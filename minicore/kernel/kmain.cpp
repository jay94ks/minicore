#include "acpi.h"
#include "address_space.h"
#include "async_task.h"
#include "boot_info.h"
#include "channel.h"
#include "debug_session.h"
#include "delayed_exec.h"
#include "devmgr_service.h"
#include "fs_service.h"
#include "dma_buffer.h"
#include "gdt.h"
#include "hvm_start_info.h"
#include "idt.h"
#include "interrupt_subscription.h"
#include "libcpio/cpio.h"
#include "libelf/elf.h"
#include "libkenv/mem.h"
#include "libkenv/types.h"
#include "ioapic.h"
#include "lapic.h"
#include "livefs.h"
#include "logger.h"
#include "mount_table.h"
#include "multiboot2.h"
#include "named_object.h"
#include "libkmm/slab.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "pci.h"
#include "pnp.h"
#include "power.h"
#include "process.h"
#include "rcu.h"
#include "resource_group.h"
#include "deferred_destruction.h"
#include "scheduler.h"
#include "serial.h"
#include "smp.h"
#include "syscall.h"
#include "syscall_fastpath.h"
#include "task.h"
#include "timer.h"
#include "tlb_shootdown.h"
#include "user_record.h"
#include "user_sync.h"
#include "vfs_syscall.h"

namespace {

// boot.S가 esi로 넘기는 값(saved_boot_protocol, 0=PVH/1=multiboot2) -
// 두 부팅 경로 모두 Idt::init() 이후로는 완전히 같은 코드를 탄다
// (PL-FC38956C). PVH는 이 값의 기본값(0)이자 "else" 케이스로 처리
// 한다 - 별도 상수를 안 둔 건 boot.S가 인식 못하는 값을 보낼 방법이
// 없어서(두 진입점만 존재) 대칭적인 분기가 오히려 불필요.
constexpr kernel::uint32_t kBootProtocolMultiboot2 = 1;

void kLogPciDevice(const kernel::Pci::Device& dev) {
    kernel::Logger::info("  pci %x:%x.%x vendor=%x device=%x class=%x subclass=%x", dev.bus, dev.device, dev.function,
                          dev.vendorId, dev.deviceId, dev.classCode, dev.subclass);
}

// linker.ld가 정의하는 커널 자신의 물리 범위 - usable 메모리에서
// 제외하는 데 쓴다(page_frame_allocator.cpp).
extern "C" char kernel_phys_start[];
extern "C" char kernel_phys_end[];

// 커널 커맨드라인에서 flag(예: "--disable-x2apic")를 찾는다 - 표준
// strstr이 freestanding에 없어 직접 구현(libkenv에 문자열 유틸리티가
// 아직 없음 - QU-19B76E06 open, 답변 오면 그쪽으로 옥길 수 있음).
bool kCmdlineHasFlag(const char* cmdline, const char* flag) {
    if (!cmdline) {
        return false;
    }
    for (const char* p = cmdline; *p; ++p) {
        const char* a = p;
        const char* b = flag;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

// initrd 안의 "init"(SP-68182FBD "initrd 레이아웃: 서브디렉토리 없이
// 전부 루트에 평면 배치", "커널이 최초로 구동할 유저영역 프로그램은
// init 하나로 하드코딩")을 이 버퍼로 복사해 둔다. **왜 여기서 즉시
// 복사하는가(QU-A7D8E49B 설계자 답변, 2026-09-15)**: PageFrameAllocator
// 는 부트 모듈의 물리 범위를 예약 목록에 넣지 않아(kLowReservedEnd/
// 커널 자신/start_info/memmap 배열 넷뿐, page_frame_allocator.cpp)
// 그 프레임이 나중에 버디 할당기로 재할당돼 덮어쓰일 수 있다 - 이
// 버퍼가 커널 자신의 BSS 안에 있으므로(=kernelPhysStart..End 안에
// 있으므로) 그 예약에 자동으로 포함돼 별도 처리가 필요 없다. 그래서
// 실제로 다시 쓰기 전(PageFrameAllocator::init() 호출보다도 먼저,
// kLogBootInfo가 호출되는 이 시점)에 필요한 바이트만 뿑아 두고,
// 모듈의 원본 물리 페이지는 그 뒤로 일반 usable 메모리처럼 재활용돼도
// 안전하다.
constexpr kernel::uint64_t kMaxInitImageSize = 1UL * 1024UL * 1024UL;  // 1MiB v1 상한(실측 후 조정, RM-23F4B687 §4)
kernel::uint8_t gInitImageBuffer[kMaxInitImageSize];
kernel::uint64_t gInitImageSize = 0;
bool gInitImageFound = false;

// 부팅 매니페스트(SP-EAB162FC §2.2, PN-D3C05C0B) - initrd 안에서
// "net"/"tty"/"pubreg"/"authmgr"라는 정확한 이름과 일치하는 실행 파일을 찾아
// ProcessRole::KernelService로 스폰하는 고정 이름 목록("pubreg"는
// [추가, 2026-09-16, 설계자 지시 - SP-B071E628 "프로세스간 공개
// 인터페이스" 재설계로 5번째 커널 서비스 신설], SP-EAB162FC §2.2가
// 이미 이 다섯 이름을 공식화해 둠 - PN-185406F6 항목1). **[제외,
// 2026-09-20, SP-43331889/QU-23B339AB/QU-ECEE5990/QU-5FC58B06]
// "devmgr"/"fs" 둘 다 더 이상 이 ELF 기반 매니페스트에 없다** -
// Process 없는 순수 커널 KernelThread로 완전 흡수돼(아래
// kSpawnServiceProcesses() 호출부 근처 kSpawnDevmgrKernelThread()/
// kSpawnFsKernelThread() 참고) initrd 안에 실행 파일 자체가
// 없어졌다(scripts/build-initrd.sh도 더 이상 이 둘을 담지 않음).
// **[추가, 2026-09-20, SP-8B6B8D25 §3.1 항목8, PN-24A2B6F5/
// PN-CFEAEF40]** "authmgr"(6번째 커널 서비스, 사용자 신원 관리)이
// pubreg와 동일한 이유로 이 목록에 새로 추가됐다 - v1은 스캐폴딩만
// (PN-CFEAEF40 범위, 실제 프로토콜은 후속 세션).
// "init"과 완전히 같은 물리 메모리 안전성 이유(위 gInitImageBuffer
// 문서 주석 참고 - PageFrameAllocator::init() 이전에 커널 BSS 안으로
// 복사해 둬야 그 예약 범위에 자동으로 포함된다)로 각자 전용 정적
// 버퍼를 쓴다. v1은 이 이름들 각각 정확히 하나의 인스턴스만 지원
// (여러 개가 있으면 마지막으로 매치된 것만 남는다 - 지금은 문제되지
// 않음, 실제로 여러 인스턴스가 필요해지면 재검토).
// [추가, 2026-09-20, SP-8B6B8D25 §3.1 항목8, PN-24A2B6F5/PN-CFEAEF40]
// "authmgr"(6번째 커널 서비스, 사용자 신원 관리) - v1은 스캐폴딩만
// (PN-CFEAEF40 범위) - 실제 프로토콜 처리는 minicore/authmgr/main.cpp가
// accept 직후 즉시 close하는 상태라, 이 매니페스트 항목이 있어도 아직
// 유의미한 요청 왕복은 일어나지 않는다.
constexpr kernel::uint32_t kServiceManifestCount = 4;
kernel::uint8_t gNetImageBuffer[kMaxInitImageSize];
kernel::uint8_t gTtyImageBuffer[kMaxInitImageSize];
kernel::uint8_t gPubregImageBuffer[kMaxInitImageSize];
kernel::uint8_t gAuthmgrImageBuffer[kMaxInitImageSize];

struct ServiceManifestEntry {
    const char* name;
    kernel::uint32_t nameLength;  // cpio::Entry::nameLength와 같은 규약(널 제외)
    kernel::uint8_t* buffer;
    kernel::uint64_t size = 0;
    bool found = false;
};

ServiceManifestEntry gServiceManifest[kServiceManifestCount] = {
    {"net", 3, gNetImageBuffer},
    {"tty", 3, gTtyImageBuffer},
    {"pubreg", 6, gPubregImageBuffer},
    {"authmgr", 7, gAuthmgrImageBuffer},
};

// 부팅 모듈(initrd)이 있으면 libcpio로 훑어 로그를 남기고(QU-9DCDCE3E -
// "initrd 역시도 마찬가지다"), 그중 이름이 "init" 또는 부팅 매니페스트
// (위 gServiceManifest)와 일치하는 파일이 있으면 각자 버퍼로 즉시
// 복사해 둔다(PN-DF4E626D/PN-D3C05C0B). 실제 마운트 가능한 파일시스템이
// 아직 없어 이 다섯 이름 외 나머지 파일은 여전히 진단 로그까지만
// 한다 - fs 서비스가 생기면 이 파서를 그대로 재사용.
void kLogCpioEntry(const cpio::Entry& entry, void*) {
    // entry.name은 아카이브 안의 파일명 바이트를 그대로 가리킨다 -
    // nameSize(원본 필드)가 null 포함이라 name[nameLength]가 이미
    // '\0'이므로 별도 복사 없이 그대로 null-terminated 문자열이다.
    kernel::Logger::info("    cpio: %s size=%lx mode=%x", entry.name, entry.dataSize, entry.mode);

    if (!entry.data) {
        return;
    }

    if (!gInitImageFound && entry.nameLength == 4 && entry.name[0] == 'i' && entry.name[1] == 'n' &&
        entry.name[2] == 'i' && entry.name[3] == 't') {
        if (entry.dataSize > kMaxInitImageSize) {
            kernel::Logger::warn("minicore: init image exceeds kMaxInitImageSize - skipping load");
            return;
        }
        memcpy(gInitImageBuffer, entry.data, entry.dataSize);
        gInitImageSize = entry.dataSize;
        gInitImageFound = true;
        return;
    }

    for (kernel::uint32_t i = 0; i < kServiceManifestCount; ++i) {
        ServiceManifestEntry& svc = gServiceManifest[i];
        if (svc.found || entry.nameLength != svc.nameLength) {
            continue;
        }
        bool matches = true;
        for (kernel::uint32_t j = 0; j < svc.nameLength; ++j) {
            if (entry.name[j] != svc.name[j]) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }
        if (entry.dataSize > kMaxInitImageSize) {
            kernel::Logger::warn("minicore: service image exceeds kMaxInitImageSize - skipping load: %s", svc.name);
            return;
        }
        memcpy(svc.buffer, entry.data, entry.dataSize);
        svc.size = entry.dataSize;
        svc.found = true;
        return;
    }
}

void kLogBootInfo(const kernel::BootInfo& bootInfo) {
    kernel::Logger::info("minicore: cmdline=%s", bootInfo.cmdline ? bootInfo.cmdline : "(none)");
    if (bootInfo.bootloaderName) {
        kernel::Logger::info("minicore: bootloader=%s", bootInfo.bootloaderName);
    }
    kernel::Logger::info("minicore: modules=%x", bootInfo.moduleCount);
    for (kernel::uint32_t i = 0; i < bootInfo.moduleCount; ++i) {
        const kernel::BootModule& mod = bootInfo.modules[i];
        kernel::Logger::info("  module[%x] phys=%llx-%llx cmdline=%s", i, mod.physStart, mod.physEnd,
                              mod.cmdline ? mod.cmdline : "(none)");

        // 모듈을 CPIO(newc) 아카이브로 시도해 본다 - 매직이 안 맞으면
        // forEachEntry가 즉시 0을 반환하므로 CPIO가 아닌 모듈(예: 커널
        // 자체 설정 파일)이어도 안전하다. 물리주소를 그대로 포인터로
        // 캐스팅한다(Paging::init() 이전, 저지대 identity map 범위).
        const auto* archive = reinterpret_cast<const void*>(mod.physStart);
        const kernel::uint64_t archiveSize = mod.physEnd - mod.physStart;
        cpio::forEachEntry(archive, archiveSize, kLogCpioEntry, nullptr);

        // /sys/live/initrd.cpio(SP-7CC5693A §2.4, PN-71C2B857) - 개별
        // 엔트리만 뽑아 담는 gInitImageBuffer류와 달리 아카이브 원본
        // 바이트 전체를 그대로 보존해야 한다(하나의 불투명한 파일로
        // 노출하므로) - PageFrameAllocator::init()보다 먼저인 지금
        // 복사해 둬야 안전한 이유는 gInitImageBuffer 문서 주석과 동일
        // (QU-A7D8E49B). CPIO가 아닌 모듈이어도 captureCpioArchive()가
        // 크기 상한만 확인하고 그대로 복사하지만, 첫 모듈만 채택되고
        // 실제 initrd 부팅 시나리오는 모듈이 하나뿐이라 문제되지 않는다.
        kernel::LiveFs::captureCpioArchive(archive, archiveSize);
    }
}

void kLogMemoryMap(const kernel::HvmMemmapEntry* memmap, kernel::uint32_t count) {
    kernel::Logger::info("minicore: memory map (%x entries)", count);
    for (kernel::uint32_t i = 0; i < count; ++i) {
        kernel::Logger::info("  base=%llx size=%llx type=%x", memmap[i].addr, memmap[i].size, memmap[i].type);
    }
}

// usable 영역들의 최대 끝 주소 - Paging::init()이 direct map으로 덤어야
// 할 실제 설치 메모리 크기다(PN-4AA5425D, PageFrameAllocator::init()이
// usable 타입만 프레임 풀에 넣는 것과 같은 기준으로 usable만 본다).
kernel::uint64_t kComputeMaxUsablePhysAddr(const kernel::HvmMemmapEntry* memmap, kernel::uint32_t count) {
    kernel::uint64_t maxAddr = 0;
    for (kernel::uint32_t i = 0; i < count; ++i) {
        if (memmap[i].type != static_cast<kernel::uint32_t>(kernel::HvmMemmapType::kUsable)) {
            continue;
        }
        const kernel::uint64_t end = memmap[i].addr + memmap[i].size;
        if (end > maxAddr) {
            maxAddr = end;
        }
    }
    return maxAddr;
}

// 실제 첫 프로세스 기동(QA-26450C3E "유저랜드 준비", PN-16CA347D 6번
// 마지막 조각, PN-DF4E626D) - gInitImageFound가 세팅돼 있으면(위
// kLogCpioEntry가 이미 부팅 극초반에 채워 둘) 그 ELF를 파싱해 실제
// Process/UserThread로 ring3 진입시킨다(Process::execImage/kEnterRing3,
// PN-55D24891). PageFrameAllocator/Scheduler/AsyncReactor가 전부 준비된
// 뒤(=이 함수 호출 시점)에만 안전하다 - execImage가 유저 스택 페이지를
// 확보하고 UserThread::init()이 커널 스택을 확보하기 때문이다.
elf::Image gInitImage;
// [수정, 2026-09-17, PN-E2A114C1, DC-21647E46/QU-76409699 "(B) 포함으로
// 읽자"] 진짜 정적 전역 `Process`에서 `SharedPtr<Process>`로 전환 -
// 이제 다른 모든 Process 인스턴스와 동일하게 `Process::allocate()`
// (슬랩 할당 + memset(0)) 위에서 `kMakeShared`로 감싼다.
kernel::SharedPtr<kernel::Process> gInitProcess;
kernel::UserThread gInitThread;

void kSpawnInitProcess() {
    if (!gInitImageFound) {
        kernel::Logger::info("minicore: no \"init\" entry found in initrd modules - skipping first process spawn");
        return;
    }
    if (elf::Image::parse(gInitImageBuffer, gInitImageSize, &gInitImage) != elf::Error::None) {
        kernel::Logger::error("minicore: init image ELF parse FAILED");
        return;
    }
    kernel::Process* raw = kernel::Process::allocate();
    if (!raw) {
        kernel::Logger::error("minicore: init process allocation FAILED");
        return;
    }
    if (!raw->init()) {
        kernel::Process::release(raw);
        kernel::Logger::error("minicore: init process address space allocation FAILED");
        return;
    }
    // [실측/코드 추적으로 발견한 잠재 회귀, 2026-09-17, PN-E2A114C1]
    // `gInitProcess`가 진짜 정적 전역이던 시절엔 `ProcessStartFlags::
    // essential`의 NSDMI(`true`)가 실제 C++ 정적 초기화로 적용됐다 -
    // "커널 서비스가 죽으면 패닉"이라는 안전장치가 그 암묵적 보장
    // 하나에 의존하고 있었다는 뜻. 이제 `Process::allocate()`의
    // `memset(0)` 경로를 타면서(다른 모든 동적 Process와 동일) 그
    // 암묵적 `true`가 조용히 `false`로 뒤집힐 뻔했다 - 명시적으로
    // 다시 세팅해 기존 동작을 그대로 보존한다.
    raw->startFlags.essential = true;
    kernel::SharedPtr<kernel::Process> proc = kernel::kMakeShared<kernel::Process>(raw);
    if (!proc) {
        kernel::Logger::error("minicore: init process control block allocation FAILED");
        raw->destroy();
        kernel::Process::release(raw);
        return;
    }
    gInitProcess = proc;
    // [신규, 2026-09-16, SP-6BEAE0C1 §6, PN-543C0CE9 착수 5번째 증분(2/2)]
    // gInitProcess를 고아 입양 대상(orphan root)으로 등록 - init()이
    // 성공해 pml4Phys/addressSpace가 진짜로 유효해진 직후, 하지만
    // execImage()가 유저 스택/ELF 로드를 시도하기 전에 먼저 해 둔다
    // (이 등록 자체는 그 이후 단계들의 성패와 무관 - "이 Process가
    // 유효한 좀비 트리 루트다"라는 사실만 필요하다).
    kernel::Process::setOrphanRoot(gInitProcess);
    // [신규, 2026-09-17, SP-245D130B §1] init은 트리 루트라 부모가 없어
    // joinResourceGroup()의 "부모 그룹 상속" 기본값을 못 쓴다 - 명시적
    // 으로 루트 자원 그룹에 가입시킨다(SpawnProcess로 만들어질 그
    // 자손들은 이 값을 그대로 상속받게 됨, process.cpp SpawnProcessHandler
    // 참고).
    gInitProcess->joinResourceGroup(&kernel::gRootResourceGroup);
    // spawnName(PN-71C2B857, SP-00CA7175 §2.0) - role/startFlags와 같은
    // 관례로 init() 직후 호출부가 직접 채운다.
    memcpy(gInitProcess->spawnName, "init", 4);
    gInitProcess->spawnNameLen = 4;
    kernel::UserThread* thread = gInitProcess->execImage(gInitImage, &gInitThread);
    if (!thread) {
        kernel::Logger::error("minicore: init process execImage FAILED");
        gInitProcess.reset();
        return;
    }
    kernel::Scheduler::enqueue(kernel::Scheduler::currentCoreIndex(), thread);
    kernel::Logger::info("minicore: init process spawned, entry=%llx", gInitImage.entryPoint());
}

// 부팅 매니페스트(SP-EAB162FC §2.2, PN-D3C05C0B) - devmgr/fs/net/tty
// 중 initrd에 실제로 존재하는 것만 ProcessRole::KernelService로
// 스폰한다. "init"과 달리 하나라도 없다고 부팅을 막지 않는다(로그만
// 남기고 계속 진행 - 이 넷은 아직 실제 구현이 하나도 없으므로
// initrd에 없는 게 v1의 정상 상태다).
elf::Image gServiceImage[kServiceManifestCount];
// [수정, 2026-09-17, PN-E2A114C1] gInitProcess와 동일한 이유로
// SharedPtr<Process> 배열로 전환.
kernel::SharedPtr<kernel::Process> gServiceProcess[kServiceManifestCount];
kernel::UserThread gServiceThread[kServiceManifestCount];

void kSpawnServiceProcesses() {
    for (kernel::uint32_t i = 0; i < kServiceManifestCount; ++i) {
        const ServiceManifestEntry& svc = gServiceManifest[i];
        if (!svc.found) {
            kernel::Logger::info("minicore: no \"%s\" entry found in initrd modules - skipping service spawn",
                                  svc.name);
            continue;
        }
        if (elf::Image::parse(svc.buffer, svc.size, &gServiceImage[i]) != elf::Error::None) {
            kernel::Logger::error("minicore: service image ELF parse FAILED: %s", svc.name);
            continue;
        }
        kernel::Process* raw = kernel::Process::allocate();
        if (!raw) {
            kernel::Logger::error("minicore: service process allocation FAILED: %s", svc.name);
            continue;
        }
        if (!raw->init()) {
            kernel::Process::release(raw);
            kernel::Logger::error("minicore: service process address space allocation FAILED: %s", svc.name);
            continue;
        }
        // role은 스폰 시점에 고정(SP-EAB162FC §1/§2.2 - 이후 바꾸는
        // API를 두지 않는다는 원칙 그대로, execImage 이전에 채운다).
        raw->role = kernel::ProcessRole::KernelService;
        // [실측/코드 추적으로 발견한 잠재 회귀, 2026-09-17, PN-E2A114C1]
        // kSpawnInitProcess()와 동일한 이유 - 진짜 정적 전역이던 시절의
        // 암묵적 `essential=true`를 명시적으로 복원한다.
        raw->startFlags.essential = true;
        // spawnName(PN-71C2B857, SP-00CA7175 §2.0) - LiveFs::open(
        // "kernel/<name>")이 호출자가 정말 그 이름의 서비스 자신인지
        // 검사하는 데 쓴다(role과 같은 관례).
        memcpy(raw->spawnName, svc.name, svc.nameLength);
        raw->spawnNameLen = svc.nameLength;
        kernel::SharedPtr<kernel::Process> proc = kernel::kMakeShared<kernel::Process>(raw);
        if (!proc) {
            kernel::Logger::error("minicore: service process control block allocation FAILED: %s", svc.name);
            raw->destroy();
            kernel::Process::release(raw);
            continue;
        }
        gServiceProcess[i] = proc;
        // [신규, 2026-09-17, SP-245D130B §1] kSpawnInitProcess()와 동일한
        // 이유 - 고정 스폰 KernelService도 SpawnProcess 경로를 안 타므로
        // 부모 그룹 상속 기본값을 못 쓴다, 명시적으로 루트에 가입.
        gServiceProcess[i]->joinResourceGroup(&kernel::gRootResourceGroup);
        kernel::UserThread* thread = gServiceProcess[i]->execImage(gServiceImage[i], &gServiceThread[i]);
        if (!thread) {
            kernel::Logger::error("minicore: service process execImage FAILED: %s", svc.name);
            gServiceProcess[i].reset();
            continue;
        }
        kernel::Scheduler::enqueue(kernel::Scheduler::currentCoreIndex(), thread);
        kernel::Logger::info("minicore: service process spawned (KernelService): %s, entry=%llx", svc.name,
                              gServiceImage[i].entryPoint());

        // Tier A/B 예약(SP-00CA7175 §2.0, PN-7AC01E6E 항목 4) - 이
        // 서비스가 나중에 livefs를 통해 `/sys/live/kernel/<name>`을 열면
        // (아직 livefs 자체 미구현 - PN-71C2B857) 이 자리를 받아간다.
        if (!kernel::KernelReservedTable::reserveForKernelService(svc.name, svc.nameLength)) {
            kernel::Logger::warn("minicore: kernel-reserved slot allocation FAILED (non-fatal): %s", svc.name);
        }
    }
}

// [신규, 2026-09-20, SP-43331889 §7] devmgr을 Process 없는 순수 커널
// `KernelThread`로 직접 스폰한다 - 위 kSpawnServiceProcesses()의 ELF
// 매니페스트 경로(net/tty/pubreg)와 달리 initrd/ELF 로드가 전혀
// 없다(kernel::kDevmgrKernelMain은 그냥 함수 포인터, devmgr.cpp 문서
// 주석 참고). ProcessRole/essential 같은 Process
// 전용 개념도 없다 - §7-1의 무한 대기 루프가 유일한 안전장치.
void kSpawnDevmgrKernelThread() {
    kernel::KernelThread* thread = kernel::kSpawnKernelThread(kernel::kDevmgrKernelMain, nullptr);
    if (!thread) {
        kernel::Logger::error("minicore: devmgr KernelThread allocation FAILED");
        return;
    }
    kernel::Scheduler::enqueue(kernel::Scheduler::currentCoreIndex(), thread);
    kernel::Logger::info("minicore: devmgr KernelThread spawned (Process-less)");
}

// [신규, 2026-09-20, SP-43331889 §7, QU-5FC58B06] fs도 devmgr과
// 완전히 동일한 방식으로 Process 없는 순수 커널 `KernelThread`로
// 직접 스폰한다 - fs.cpp 문서 주석 참고.
void kSpawnFsKernelThread() {
    kernel::KernelThread* thread = kernel::kSpawnKernelThread(kernel::kFsKernelMain, nullptr);
    if (!thread) {
        kernel::Logger::error("minicore: fs KernelThread allocation FAILED");
        return;
    }
    kernel::Scheduler::enqueue(kernel::Scheduler::currentCoreIndex(), thread);
    kernel::Logger::info("minicore: fs KernelThread spawned (Process-less)");
}

// [신규, 2026-09-23, DC-F367AD5D/SP-0C7A4F3B §1 항목5] ACPI 전원
// 버튼(SCI) 감시 - devmgr/fs와 동일한 Process 없는 KernelThread
// 패턴이나, 애초에 자동 종료를 시작할 방법이 없는 하드웨어(FADT가
// 없거나/SCI_INT가 0이거나/`\_S5` 패키지를 못 찾았거나)에서는 스레드
// 자체를 스폰하지 않는다 - devmgr/fs가 "장치 없음"일 때 자기 안에서
// 계속 대기하는 것과 달리, 이쪽은 조건을 boot 시점에 이미 다 알 수
// 있어 애초에 만들지 않는 편이 낫다고 판단(RM-23F4B687 §4).
//
// **[비활성화, 2026-09-23, PN-0B461E6F, 재현율 서술 정정]** 이
// 스레드가 존재하는 것만(실제 전원 버튼 이벤트가 한 번도 안 와도)
// 표준 회귀 GRUB SMP4에서 매우 높은 빈도로 PANIC(Invalid Opcode/
// Page Fault, core=1, 버튼 입력 자체는 시도 안 한 순수 부팅만으로도
// 발생)을 유발함을 실측 확인 - 반복 재현 시도 중 크래시 없이 그냥
// 멈춘 사례가 나와 "100% 결정론적"이 아니라 "매우 높은 빈도의
// heisenbug"로 재분류했다(PN-0B461E6F 참고, 정확한 재현율은 미확정).
// 이 프로젝트가 이미 추적 중인 AHCI+SMP4 인터럽트 서브시스템 재진입
// 계열(PN-E4C6AF72/PN-3DDF2797)과 같은 근본 원인일 가능성이 높다
// (InterruptDelegation::allow()로 새 IOAPIC 외부 인터럽트를 구독하는
// 첫 실사용처가 AHCI 말고 이걸로는 처음이라, 그 자체가 취약점을
// 다시 노출시킨 것으로 추정 - 확정은 못 함. 더 구체적인 가설:
// `AsyncTask::submit(preemptive=false)`+`AsyncTaskWaitGroup::
// waitAll()` 패턴 자체가 SMP4에서 위험하다는 쪽이 SCI/ACPI 특정
// 원인보다 더 근본적일 수 있음 - PN-0B461E6F 참고). SMP1은 표준 4시나리오
// 전부(PVH/GRUB SMP1/직접 shutdown() 호출/실제 QEMU `system_powerdown`
// 모니터 명령으로 진짜 ACPI 전원 버튼 이벤트까지) 완전히 무결함을
// 실측 검증했다 - 이 스레드를 스폰하는 호출 한 줄만 주석 처리해
// 원래 안전한 상태로 되돌린다. Shutdown/Reboot syscall(그룹10)은
// 이 스레드와 무관하게 완전히 독립적으로 동작하므로 영향 없음.
// 재활성화하려면 아래 호출부의 주석만 풀면 되지만, 그 전에
// SMP4에서 이 크래시의 근본 원인부터 규명할 것 - 자세한 내용은
// PN-0B461E6F 참고.
[[maybe_unused]] void kSpawnPowerKernelThreadIfSupported() {
    if (!kernel::Acpi::hasFadt() || kernel::Acpi::sciInterruptGsi() == 0 || !kernel::Power::hasS5()) {
        kernel::Logger::info("minicore: ACPI power button watch not available - skipping (no FADT/SCI/\\_S5)");
        return;
    }
    kernel::KernelThread* thread = kernel::kSpawnKernelThread(kernel::kPowerKernelMain, nullptr);
    if (!thread) {
        kernel::Logger::error("minicore: power KernelThread allocation FAILED");
        return;
    }
    kernel::Scheduler::enqueue(kernel::Scheduler::currentCoreIndex(), thread);
    kernel::Logger::info("minicore: power KernelThread spawned (Process-less, ACPI power button watch)");
}

}  // namespace

// boot.S가 higher-half로 넘어온 뒤 호출한다. rdi = 부팅 정보 구조체
// (PVH면 hvm_start_info, multiboot2면 그 정보 구조체)의 물리 주소,
// rsi = 어느 프로토콜인지(kBootProtocolPvh/kBootProtocolMultiboot2,
// EBX/EAX로 전달된 값을 boot.S가 저장해 듬다가 넘김, PL-FC38956C).
// 이 함수 맨 위에서 프로토콜별로 memmap/rsdpPaddr를 같은 형태
// (HvmMemmapEntry 배열 + 물리주소)로 통일하고 나면, 그 뒤부터는 완전히
// 프로토콜 무관 공통 경로다. 이 시점에는 커널(ring 0)만 실행 중이다 -
// devmgr 등 "커널 서비스"는 아직 존재하지 않는다(SP-8B6B8D25 §2-A).
// [신규, 2026-09-17, x86_64-elf-gcc 크로스컴파일 툴체인 교체 중 실측
// 발견] 전역 C++ 생성자(.init_array) 순회 - linker.ld의 .init_array
// 출력 섹션 문서 주석 참고. BSP에서 다른 어떤 코드보다도 먼저 정확히
// 한 번만 호출해야 한다(AP는 절대 다시 부르면 안 됨 - 전역 객체를
// 두 번 생성하면 안 되므로).
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();

namespace {
void kRunGlobalConstructors() {
    for (void (**ctor)() = __init_array_start; ctor != __init_array_end; ++ctor) {
        (*ctor)();
    }
}
}  // namespace

extern "C" void kMain(kernel::uint32_t startInfoAddr, kernel::uint32_t bootProtocol) {
    kRunGlobalConstructors();
    kernel::Serial::init();
    kernel::Logger::init();

    static kernel::HvmMemmapEntry gMb2MemmapBuffer[kernel::kMultiboot2MaxMemmapEntries];
    const kernel::HvmMemmapEntry* memmap = nullptr;
    kernel::uint32_t memmapEntries = 0;
    kernel::uint64_t rsdpPaddr = 0;
    kernel::uint64_t startInfoSize = 0;
    kernel::BootInfo bootInfo{};
    // SP-CC2B18C6 §2 - 커널이 KERNEL_LMA(1MiB)가 아닌 물리주소에 로드된
    // 경우의 보정값. GRUB/PVH는 항상 1MiB 고정 로드라 0(무회귀) - UEFI
    // 직접 부팅 경로(PN-7FBF255A)가 도입되면 이 분기에서 실제 값을 채운다.
    kernel::uint64_t physicalBaseDelta = 0;

    if (bootProtocol == kBootProtocolMultiboot2) {
        kernel::Logger::info("minicore: booted via multiboot2 (GRUB, higher-half, long mode)");
        kernel::uint32_t mb2TotalSize = 0;
        kernel::Multiboot2Info::parse(static_cast<kernel::uint64_t>(startInfoAddr), gMb2MemmapBuffer,
                                       kernel::kMultiboot2MaxMemmapEntries, &memmapEntries, &rsdpPaddr, &mb2TotalSize,
                                       &bootInfo);
        memmap = gMb2MemmapBuffer;
        startInfoSize = mb2TotalSize;
    } else {
        kernel::Logger::info("minicore: booted via Xen PVH (higher-half, long mode)");
        const auto* startInfo = reinterpret_cast<const kernel::HvmStartInfo*>(static_cast<kernel::uint64_t>(startInfoAddr));
        if (startInfo->magic == kernel::kHvmStartInfoMagic) {
            kernel::Logger::info("minicore: hvm_start_info magic OK");
        } else {
            kernel::Logger::error("minicore: hvm_start_info magic MISMATCH");
        }
        memmap = reinterpret_cast<const kernel::HvmMemmapEntry*>(startInfo->memmapPaddr);
        memmapEntries = startInfo->memmapEntries;
        rsdpPaddr = startInfo->rsdpPaddr;
        startInfoSize = sizeof(kernel::HvmStartInfo);

        // PVH도 QU-9DCDCE3E 지시대로 커맨드라인/모듈을 채운다 -
        // hvm_start_info가 이미 두 필드를 갖고 있었다(cmdlinePaddr/
        // modlistPaddr+nrModules) - 부트로더 이름 개념은 PVH ABI에
        // 없어 항상 nullptr로 남는다.
        bootInfo.cmdline = startInfo->cmdlinePaddr
                               ? reinterpret_cast<const char*>(startInfo->cmdlinePaddr)
                               : nullptr;
        bootInfo.bootloaderName = nullptr;
        bootInfo.moduleCount = 0;
        const kernel::uint32_t moduleCount =
            startInfo->nrModules < kernel::kBootInfoMaxModules ? startInfo->nrModules : kernel::kBootInfoMaxModules;
        if (startInfo->modlistPaddr) {
            const auto* modlist = reinterpret_cast<const kernel::HvmModlistEntry*>(startInfo->modlistPaddr);
            for (kernel::uint32_t i = 0; i < moduleCount; ++i) {
                kernel::BootModule& mod = bootInfo.modules[bootInfo.moduleCount];
                mod.physStart = modlist[i].paddr;
                mod.physEnd = modlist[i].paddr + modlist[i].size;
                mod.cmdline = modlist[i].cmdlinePaddr ? reinterpret_cast<const char*>(modlist[i].cmdlinePaddr) : nullptr;
                ++bootInfo.moduleCount;
            }
        }
    }

    kernel::Gdt::init();
    kernel::Logger::info("minicore: GDT ready");

    kernel::Idt::init();
    kernel::Logger::info("minicore: IDT ready");

    kLogBootInfo(bootInfo);
    if (kCmdlineHasFlag(bootInfo.cmdline, "--disable-x2apic")) {
        kernel::Lapic::setX2ApicDisabled(true);
        kernel::Logger::info("minicore: --disable-x2apic requested, x2APIC will be forced off");
    }

    kLogMemoryMap(memmap, memmapEntries);

    // 순서 중요: Paging(direct map) -> Acpi(SRAT로 NUMA 토폴로지 확보,
    // direct map으로 테이블을 읽음) -> PageFrameAllocator(Acpi의 NUMA
    // 정보로 노드별 buddy 구성) -> Lapic(PageFrameAllocator에서 페이지
    // 테이블용 프레임을 받아옴 - 그 안에서 자기 자신의 id()를 부르지
    // 않도록 Lapic::isReady()로 방어돼 있음, 2026-09-14 실측으로
    // 발견한 초기화 순서 문제).
    kernel::Paging::init(kComputeMaxUsablePhysAddr(memmap, memmapEntries), physicalBaseDelta);
    // [신규, 2026-09-18, SP-8D206F11 §2.2] IA32_PAT는 코어별 MSR이라
    // BSP도 자기 몫을 스스로 설정해야 한다(kApMain이 AP 몫을 설정 -
    // smp.cpp 참고, SyscallFastPath::initForThisCore()와 동일한 관례).
    kernel::Paging::initPatForThisCore();
    kernel::Logger::info("minicore: direct physical map ready");

    if (kernel::Acpi::init(rsdpPaddr)) {
        kernel::Logger::info(
            "minicore: ACPI MADT/SRAT parsed, cpu_count=%x numa_nodes=%x local_apic_addr=%llx ioapic_count=%x hpet=%s",
            kernel::Acpi::cpuCount(), kernel::Acpi::numaNodeCount(), kernel::Acpi::localApicAddress(),
            kernel::Acpi::ioApicCount(), kernel::Acpi::hasHpet() ? "yes" : "no");
        for (kernel::uint32_t i = 0; i < kernel::Acpi::cpuCount(); ++i) {
            kernel::Logger::info("  cpu[%x] apic_id=%x numa_node=%x", i, kernel::Acpi::cpuApicId(i),
                                  kernel::Acpi::cpuNumaNode(i));
        }
        for (kernel::uint32_t i = 0; i < kernel::Acpi::ioApicCount(); ++i) {
            kernel::Logger::info("  ioapic[%x] id=%x addr=%x gsi_base=%x", i, kernel::Acpi::ioApicId(i),
                                  kernel::Acpi::ioApicAddress(i), kernel::Acpi::ioApicGsiBase(i));
        }
    } else {
        kernel::Logger::error("minicore: ACPI MADT parse FAILED");
    }

    // [신규, 2026-09-23, DC-F367AD5D/SP-0C7A4F3B] Acpi::init()이 FADT/
    // DSDT 위치를 파싱한 직후 - DSDT에서 \_S5 패키지를 스캔해 둔다.
    kernel::Power::init();

    // Acpi::cpuCount()만 있으면 되므로 여기서 바로 초기화한다(코어별
    // 큐를 만들어 두고, 실제 디스패치는 각 코어가 Scheduler::runLoop()
    // 에 들어가면서 시작된다 - PL-2D3184BC 4/5/6단계).
    kernel::Scheduler::init();
    // [신규, 2026-09-20, SP-5130284C] Scheduler::currentCoreIndex()를
    // 실제로 쓰므로(deferred_destruction.cpp) Scheduler::init() 이후에
    // 등록해야 한다 - libkenv/shared_ptr.h의 함수포인터 훅을 이때부터
    // 채워, 이 시점 이후 SharedPtr 소멸이 인터럽트 컨텍스트에서
    // 일어나도 안전하게 지연되도록 한다(PN-4137C88C).
    kernel::kInitDeferredDestruction();

    kernel::PageFrameAllocator::init(
        memmap, memmapEntries,
        reinterpret_cast<kernel::uint64_t>(kernel_phys_start) + physicalBaseDelta,
        reinterpret_cast<kernel::uint64_t>(kernel_phys_end) + physicalBaseDelta,
        static_cast<kernel::uint64_t>(startInfoAddr), startInfoSize);

    kernel::Logger::info("minicore: page frame allocator ready, nodes=%x free_pages=%llx",
                          kernel::PageFrameAllocator::numaNodeCount(), kernel::PageFrameAllocator::freePageCount());

    // PageFrameAllocator 이후, Scheduler::currentCoreIndex()/
    // PreemptionGuard를 실제로 쓰는 첫 alloc()/free() 호출(Lapic::init()
    // 이후) 전이면 아무때나 무방하다 - init() 자체는 정적 구조만
    // 채운다(SP-D7013B26).
    kernel::GenericSlabAllocator::init();
    kernel::Logger::info("minicore: slab allocator ready (libkmm)");

    kernel::Lapic::init();
    kernel::Logger::info("minicore: LAPIC ready, mode=%s id=%x", kernel::Lapic::usesX2Apic() ? "x2apic" : "xapic",
                          kernel::Lapic::id());

    // Acpi::cpuApicId()/Lapic::id()로 자기 코어 인덱스를 찾아야 해서
    // 반드시 이 둘 이후에 호출해야 한다(gdt.h 참고 - IST1을 #DF
    // 전용으로 채우고 ltr).
    kernel::Gdt::loadTssForThisCore();
    kernel::Logger::info("minicore: TSS/IST ready (core 0)");

    // PN-124C105B("syscall 명령 경로") - Lapic::id()로 코어 인덱스를
    // 찾으므로 Gdt::loadTssForThisCore()와 같은 이유로 Lapic::init()
    // 이후에만 안전하다.
    kernel::SyscallFastPath::initForThisCore();
    kernel::Logger::info("minicore: syscall fast path (STAR/LSTAR/SFMASK) ready (core 0)");

    // SP-0666DB3C §12.4-1(PN-25587A7D) - RDTSCP 지원 시 이 코어의 진짜
    // 인덱스를 IA32_TSC_AUX에 심어 Scheduler::currentCoreIndex()가
    // 이후 O(1)로 조회하게 한다. Gdt::loadTssForThisCore()와 같은 이유로
    // Lapic::init() 이후에만 안전.
    kernel::Scheduler::initCoreIndexForThisCore();

    kernel::Scheduler::startTickOnThisCore();
    // [신규, 2026-09-17, PN-907C5289] AP의 kApMain()과 대칭되는 지점 -
    // Nmi::stopAllOtherCores()가 아직 기동 안 된 코어를 NMI 대상에서
    // 제외할 수 있도록 BSP 자신도 "온라인"으로 표시해 둔다.
    kernel::Smp::markThisCoreOnline();
    // [신규, 2026-09-17, PN-495C11B7] RCU quiescent state 추적 시작 -
    // AP의 kApMain()과 대칭되는 지점.
    kernel::Rcu::initOnThisCore();
    kernel::Logger::info("minicore: scheduler tick ready (LAPIC, %xHz, vector=%x)", kernel::kSchedulerTickHz,
                          kernel::kSchedulerTickVector);

    kernel::AsyncReactor::init();
    kernel::Logger::info("minicore: async reactor ready (BSP-only IDT registration)");

    // 전역 테이블 하나뿐이라 BSP에서 딜 한 번만 - AP(kApMain)는 이걸
    // 다시 부르지 않는다(SyscallRegistry::registerHandler가 이미 쓰인
    // 슬롯에 재등록을 거부하므로 안전장치는 있지만, 애초에 호출
    // 자체를 한 곳에만 둔다).
    kernel::Channel::registerSyscallEndpoints();
    kernel::Logger::info("minicore: channel IPC syscall endpoints registered");

    // SP-30FCC8AE §1-A/PN-B6DB692C - root 엔트리를 캐시에 심어 둬야
    // 그 직후 등록되는 Setuid syscall(Process 그룹)이 항상 root
    // 판정을 성립시킬 수 있다.
    kernel::UserRecordCache::init();
    kernel::Logger::info("minicore: user record cache initialized (root entry)");

    // SP-6BEAE0C1/PN-543C0CE9 - 위 Channel 등록과 같은 이유(BSP에서
    // 한 번만).
    kernel::Process::registerSyscallEndpoints();
    kernel::Logger::info("minicore: SpawnProcess syscall endpoint registered");

    // SP-71DA77B3/PN-B3DD3D19 - 위 Channel/SpawnProcess 등록과 같은
    // 이유(BSP에서 한 번만). ISR 등록 자체는 InterruptDelegation::allow()
    // 가 벡터별로 지연 수행한다(여기서는 syscall endpoint 4개만 연다).
    kernel::InterruptSubscriptionService::registerSyscallEndpoints();
    kernel::Logger::info("minicore: interrupt subscription syscall endpoints registered");

    // SP-9DD4F3EA §3.1/§6 2단계 - devmgr(PN-BD9AAE2F)이 PCI 토폴로지를
    // 조회하는 데 쓰는 EnumerateDevices만 이 증분에서 연다(위와 같은
    // 이유로 BSP에서 한 번만 - RequestIoPermission 등 나머지 §3.2-§3.4
    // syscall은 후속 증분).
    kernel::PnpService::registerSyscallEndpoints();
    kernel::Logger::info("minicore: pnp EnumerateDevices syscall endpoint registered");

    // SP-39F18E30 §2 - 위와 같은 이유(BSP에서 한 번만). devmgr(또는 그
    // 드라이버 자식)이 AHCI(SP-C2670F69)/USB(SP-E35FD36C) 등 컨트롤러의
    // DMA 구조체용 물리적으로 연속인 메모리를 확보하는 AllocDmaBuffer/
    // FreeDmaBuffer - RM-48E1E610 그룹2(Device) call 2/3, 지금까지
    // "번호만 예약" 상태였다.
    kernel::DmaBufferService::registerSyscallEndpoints();
    kernel::Logger::info("minicore: dma buffer alloc/free syscall endpoints registered");

    // SP-0C7A4F3B - Shutdown/Reboot(RM-48E1E610 그룹10 "Power").
    kernel::PowerService::registerSyscallEndpoints();
    kernel::Logger::info("minicore: shutdown/reboot syscall endpoints registered");

    // SP-9A6D579F §3.2/§3.3 - 위와 같은 이유(BSP에서 한 번만). 이번
    // 증분은 DebugAttach/Detach + DebugSetBreakpoint(항목3/4) - 싱글
    // 스텝/Continue/GetRegisters/SetRegisters/ReadMemory/WriteMemory
    // (항목5/6)는 여전히 PN-87D6B615의 후속 증분.
    kernel::DebugSessionService::registerSyscallEndpoints();
    kernel::Logger::info("minicore: debug attach/detach/set-breakpoint syscall endpoints registered");
    // [신규, 2026-09-17, SP-9A6D579F §4] #DB ISR 소비자 등록 -
    // Idt::init()이 IDT 게이트 자체는 이미 부팅 초반에 다 만들어 뒀고
    // (SP-677210E6), 이건 그 위에 얹는 커널 내부 콜백 슬롯 하나만
    // 채우는 것뿐이라 특정 순서 의존성이 없다 - 위 syscall 등록
    // 직후에 자연스럽게 이어 붙인다.
    kernel::DebugSessionService::registerDebugCallback();
    kernel::Logger::info("minicore: #DB (hardware breakpoint) exception callback registered");

    // `/sys/live/kernel/` 예약 테이블(SP-00CA7175 §2.0, PN-7AC01E6E) -
    // Channel 서브시스템(위) 이후, kSpawnServiceProcesses()가 이 테이블에
    // 예약을 걸기 전에 초기화돼 있어야 한다.
    kernel::KernelReservedTable::init();
    kernel::Logger::info("minicore: kernel-reserved table ready");

    // VFS 마운트 테이블(SP-7CC5693A §2.1, PN-71C2B857) - 아직 실제
    // 마운트를 거는 소비자(livefs 자체, fs 서비스의 Mount syscall)는
    // 없다 - 테이블을 빈 상태로 준비만 해 둔다(§2.3 부팅 시퀀스가
    // 요구하는 "MountTable::init() 직후" 시점 확보).
    kernel::MountTable::init();
    kernel::Logger::info("minicore: mount table ready");

    // livefs(SP-7CC5693A §2.4, PN-71C2B857) - 어떤 유저 프로세스도 아직
    // 없는 이 시점에 커널 스스로 마운트한다(§2.4 부팅 시퀀스 그대로,
    // fs 서비스의 Mount syscall과는 다른 경로). named/kernel/
    // initrd.cpio 세 하위 경로를 이 하나의 마운트가 전부 담당한다.
    static constexpr char kLiveFsMountPath[] = "/sys/live";
    if (!kernel::MountTable::mountKernel(kLiveFsMountPath, sizeof(kLiveFsMountPath) - 1,
                                          &kernel::LiveFs::instance())) {
        kernel::Logger::error("minicore: livefs mount FAILED");
    } else {
        kernel::Logger::info("minicore: livefs mounted at /sys/live");
    }

    // VFS syscall 5종(SP-7CC5693A §2.2/§2.5, PN-452FF696) - livefs
    // 마운트 직후(위)에 이어 붙인다: fs 서비스는 아직 없지만 Mount 등을
    // 호출하려면 최소 MountTable::init()이 끝나 있어야 하므로 이 순서를
    // 지킨다(다른 registerSyscallEndpoints() 호출들과 같은 이유로
    // BSP에서 한 번만).
    kernel::VfsSyscallService::registerSyscallEndpoints();
    kernel::Logger::info("minicore: vfs mount/unmount/resolve-path/open/close/read/write syscall endpoints registered");

    // ResourceGroup syscall 6종(SP-245D130B §8/SP-6A563A8F §5-A/§7,
    // PN-4190BBD3) - 위와 같은 이유로 BSP에서 한 번만. gRootResourceGroup
    // 자체의 초기화(kResourceGroupInit())는 이 등록과 독립적이라 순서
    // 무관 - 아래쪽 kSpawnInitProcess() 직전에서 그대로 호출된다.
    kernel::ResourceGroupService::registerSyscallEndpoints();
    kernel::Logger::info("minicore: resourcegroup join/create/destroy/setcpuquota/freeze/thaw syscall endpoints registered");

    // [신규, 2026-09-18, SP-0666DB3C §17, PN-E82744B1] 위와 같은 이유로
    // BSP에서 한 번만 - 그룹 8(Sync)의 Mutex/Semaphore 8종.
    kernel::UserSyncService::registerSyscallEndpoints();
    kernel::Logger::info("minicore: mutex/semaphore create/destroy/lock/unlock/wait/post syscall endpoints registered");

    // 전역 IDT 등록이라 BSP에서 한 번만(위 registerSyscallEndpoints와
    // 같은 이유).
    kernel::TlbShootdown::init();
    kernel::Logger::info("minicore: TLB shootdown IPI handler registered");

    // KernelAddressSpaceManager(SP-2AAD7C8D §2, PN-012E8C1A) - 위
    // TlbShootdown::init() 이후에만 안전(unmapRegion()이 broadcast()를
    // 부름). 전역 싱글턴이라 BSP에서 한 번만.
    kernel::KernelAddressSpaceManager::init();
    kernel::Logger::info("minicore: kernel address space manager ready");

    kernel::IoApic::init();
    kernel::Logger::info("minicore: IOAPIC mapped");

    kernel::Timer::init();
    kernel::Logger::info("minicore: timer ready (100Hz), source=%s", kernel::Timer::usesHpet() ? "hpet" : "lapic+pit");

    // 지연 실행 큐(SP-F15B4A63, PN-C46DF296) - Timer::tickCount()를
    // 시간 기준으로 쓰므로 Timer::init() 이후, 전역 테이블 하나뿐이라
    // BSP에서 한 번만(위 KernelReservedTable::init()과 같은 이유).
    kernel::DelayedExecutionQueue::init();
    // [신규, 2026-09-22, PN-4859FDE9, SP-6CEFBE9B §8-1] swap 회수 스캔의
    // 첫 등록 - DelayedExecutionQueue::init() 이후 아무 때나(전용 Task
    // 없음, 스캔 자신이 매 실행 끝에 스스로 재등록).
    kernel::PageFrameAllocator::startReclaimScan();
    kernel::Logger::info("minicore: delayed execution queue ready, enabling interrupts");

    kernel::Pci::init();
    kernel::Logger::info("minicore: PCI config access=%s", kernel::Pci::usesMmconfig() ? "mmconfig+legacy" : "legacy");
    kernel::Logger::info("minicore: PCI enumeration:");
    kernel::Pci::enumerate(kLogPciDevice);

    // 반드시 sti 이후에 호출해야 한다(SMP AP 기동도 마찬가지 이유).
    asm volatile("sti");

    kernel::Smp::startApCores();

    // [순서 재배치, 2026-09-17, PN-9F8FF132, 설계자 지시] 이 두 호출
    // (Process::init()을 실제로 부르는 첫 지점)은 예전엔 Smp::
    // startApCores() *이전*(sti 이전)에 있었다 - 실측으로 확인된
    // PN-9F8FF132 하이젠버그(부팅 극초반 Process::init() 단 1회만으로
    // SMP4 AP 기동 자체가 멎거나 트리플 폴트하는 타이밍 의존 레이스)의
    // 재현 조건이 정확히 "AP 기동이 다 끝나기 전에 Process::init()이
    // 불린다"였다 - 정확한 레이스 지점을 계측으로 좁혀 그 자리만
    // 고치는 대신, 애초에 그 전제 조건 자체가 성립할 수 없도록 여기로
    // 옮겼다: Smp::startApCores()가 완전히 반환한(=AP 3개가 전부 기동
    // 신호를 보낸) 뒤에만 Process 관련 초기화가 실행된다. 이 순서
    // 제약은 SP-8B6B8D25/PL-65C20380 어디에도 이전엔 명시된 적이 없던
    // 새로 확정된 제약이다 - 이후 이 두 호출을 다시 Smp::startApCores()
    // 보다 앞으로 옮기지 않는다.
    // [신규, 2026-09-17, SP-245D130B §1] gRootResourceGroup은 정적
    // 전역(진짜 C++ 생성자를 거침)이라 원칙적으로 이 호출 없이도 이름
    // 없이는 쓸 수 있지만, 아래 kSpawnInitProcess()/kSpawnServiceProcesses()
    // 가 joinResourceGroup()으로 곧바로 참조하므로 이름을 채워 두는
    // 이 초기화를 명시적으로 그 직전에 호출한다(할당자 의존성 없음 -
    // SP-E9B44929 §6-A가 겪은 부팅 순서 함정과 무관).
    kernel::kResourceGroupInit();

    kSpawnInitProcess();
    kSpawnServiceProcesses();
    kSpawnDevmgrKernelThread();
    kSpawnFsKernelThread();
    // [비활성화, 2026-09-23, PN-0B461E6F] SMP4에서 100% 재현되는 PANIC
    // 발견 - kSpawnPowerKernelThreadIfSupported() 문서 주석 참고. 이
    // 한 줄만 주석 처리하면 원래 안전한 상태(SCI 감시 없음, Shutdown/
    // Reboot syscall은 그대로 동작)로 돌아간다.
    // kSpawnPowerKernelThreadIfSupported();

    // 이 지점부터 BSP 자신도 스케줄러 디스패치 루프에 들어간다 -
    // 절대 반환하지 않는다(PL-2D3184BC 5/6단계). runLoop()을 직접
    // 부르지 않는다 - enterIdleLoop()이 이 함수의 지금 이 저지대
    // 부팅 스택에서 코어 전용 안전한 idle 스택으로 먼저 옮겨 앉은
    // 뒤 그 위에서 runLoop()을 시작한다(PN-2008220B, scheduler.h
    // 문서 주석 참고).
    kernel::Scheduler::enterIdleLoop();
}
