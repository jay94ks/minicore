#include "acpi.h"
#include "address_space.h"
#include "async_task.h"
#include "boot_info.h"
#include "channel.h"
#include "gdt.h"
#include "hvm_start_info.h"
#include "idt.h"
#include "libcpio/cpio.h"
#include "libelf/elf.h"
#include "libkenv/mem.h"
#include "libkenv/types.h"
#include "ioapic.h"
#include "lapic.h"
#include "multiboot2.h"
#include "libkmm/slab.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "pci.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "smp.h"
#include "syscall.h"
#include "timer.h"
#include "tlb_shootdown.h"

namespace {

// boot.S가 esi로 넘기는 값(saved_boot_protocol, 0=PVH/1=multiboot2) -
// 두 부팅 경로 모두 Idt::init() 이후로는 완전히 같은 코드를 탄다
// (PL-FC38956C). PVH는 이 값의 기본값(0)이자 "else" 케이스로 처리
// 한다 - 별도 상수를 안 둔 건 boot.S가 인식 못 하는 값을 보낼 방법이
// 없어서(두 진입점만 존재) 대칭적인 분기가 오히려 불필요.
constexpr kernel::uint32_t kBootProtocolMultiboot2 = 1;

void kLogPciDevice(const kernel::Pci::Device& dev) {
    kernel::Serial::write("  pci ");
    kernel::Serial::writeHex(dev.bus);
    kernel::Serial::write(":");
    kernel::Serial::writeHex(dev.device);
    kernel::Serial::write(".");
    kernel::Serial::writeHex(dev.function);
    kernel::Serial::write(" vendor=");
    kernel::Serial::writeHex(dev.vendorId);
    kernel::Serial::write(" device=");
    kernel::Serial::writeHex(dev.deviceId);
    kernel::Serial::write(" class=");
    kernel::Serial::writeHex(dev.classCode);
    kernel::Serial::write(" subclass=");
    kernel::Serial::writeHex(dev.subclass);
    kernel::Serial::write("\n");
}

// linker.ld가 정의하는 커널 자신의 물리 범위 - usable 메모리에서
// 제외하는 데 쓴다(page_frame_allocator.cpp).
extern "C" char kernel_phys_start[];
extern "C" char kernel_phys_end[];

// 커널 커맨드라인에서 flag(예: "--disable-x2apic")를 찾는다 - 표준
// strstr이 freestanding에 없어 직접 구현(libkenv에 문자열 유틸리티가
// 아직 없음 - QU-19B76E06 open, 답변 오면 그쪽으로 옮길 수 있음).
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

// initrd 안의 "init"(SP-68182FBD "initrd 레이아웃: 서브디렉터리 없이
// 전부 루트에 평면 배치", "커널이 최초로 구동할 유저영역 프로그램은
// init 하나로 하드코딩")을 이 버퍼로 복사해 둔다. **왜 여기서 즉시
// 복사하는가(QU-A7D8E49B 설계자 답변, 2026-09-15)**: PageFrameAllocator
// 는 부트 모듈의 물리 범위를 예약 목록에 넣지 않아(kLowReservedEnd/
// 커널 자신/start_info/memmap 배열 넷뿐, page_frame_allocator.cpp)
// 그 프레임이 나중에 버디 할당기로 재할당돼 덮어써질 수 있다 - 이
// 버퍼가 커널 자신의 BSS 안에 있으므로(=kernelPhysStart..End 안에
// 있으므로) 그 예약에 자동으로 포함돼 별도 처리가 필요 없다. 그래서
// 실제로 다시 쓰기 전(PageFrameAllocator::init() 호출보다도 먼저,
// kLogBootInfo가 호출되는 이 시점)에 필요한 바이트만 뽑아 두고,
// 모듈의 원본 물리 페이지는 그 뒤로 일반 usable 메모리처럼 재활용돼도
// 안전하다.
constexpr kernel::uint64_t kMaxInitImageSize = 1UL * 1024UL * 1024UL;  // 1MiB v1 상한(실측 후 조정, RM-23F4B687 §4)
kernel::uint8_t gInitImageBuffer[kMaxInitImageSize];
kernel::uint64_t gInitImageSize = 0;
bool gInitImageFound = false;

// 부팅 모듈(initrd)이 있으면 libcpio로 훑어 로그를 남기고(QU-9DCDCE3E -
// "initrd 역시도 마찬가지다"), 그중 이름이 "init"인 파일이 있으면 위
// 버퍼로 즉시 복사해 둔다(PN-DF4E626D). 실제 마운트 가능한 파일시스템/
// 디바이스 관리자가 아직 없어 "init" 외 나머지 파일은 여전히 진단
// 로그까지만 한다 - 그 서브시스템이 생기면 이 파서를 그대로 재사용.
void kLogCpioEntry(const cpio::Entry& entry, void*) {
    // entry.name은 아카이브 안의 파일명 바이트를 그대로 가리킨다 -
    // nameSize(원본 필드)가 null 포함이라 name[nameLength]가 이미
    // '\0'이므로 별도 복사 없이 그대로 null-terminated 문자열이다.
    kernel::Serial::write("    cpio: ");
    kernel::Serial::write(entry.name);
    kernel::Serial::write(" size=");
    kernel::Serial::writeHex(entry.dataSize);
    kernel::Serial::write(" mode=");
    kernel::Serial::writeHex(entry.mode);
    kernel::Serial::write("\n");

    if (gInitImageFound || !entry.data || entry.nameLength != 4 ||
        entry.name[0] != 'i' || entry.name[1] != 'n' || entry.name[2] != 'i' || entry.name[3] != 't') {
        return;
    }
    if (entry.dataSize > kMaxInitImageSize) {
        kernel::Serial::write("minicore: init image exceeds kMaxInitImageSize - skipping load\n");
        return;
    }
    memcpy(gInitImageBuffer, entry.data, entry.dataSize);
    gInitImageSize = entry.dataSize;
    gInitImageFound = true;
}

void kLogBootInfo(const kernel::BootInfo& bootInfo) {
    kernel::Serial::write("minicore: cmdline=");
    kernel::Serial::write(bootInfo.cmdline ? bootInfo.cmdline : "(none)");
    kernel::Serial::write("\n");
    if (bootInfo.bootloaderName) {
        kernel::Serial::write("minicore: bootloader=");
        kernel::Serial::write(bootInfo.bootloaderName);
        kernel::Serial::write("\n");
    }
    kernel::Serial::write("minicore: modules=");
    kernel::Serial::writeHex(bootInfo.moduleCount);
    kernel::Serial::write("\n");
    for (kernel::uint32_t i = 0; i < bootInfo.moduleCount; ++i) {
        const kernel::BootModule& mod = bootInfo.modules[i];
        kernel::Serial::write("  module[");
        kernel::Serial::writeHex(i);
        kernel::Serial::write("] phys=");
        kernel::Serial::writeHex(mod.physStart);
        kernel::Serial::write("-");
        kernel::Serial::writeHex(mod.physEnd);
        kernel::Serial::write(" cmdline=");
        kernel::Serial::write(mod.cmdline ? mod.cmdline : "(none)");
        kernel::Serial::write("\n");

        // 모듈을 CPIO(newc) 아카이브로 시도해 본다 - 매직이 안 맞으면
        // forEachEntry가 즉시 0을 반환하므로 CPIO가 아닌 모듈(예: 커널
        // 자체 설정 파일)이어도 안전하다. 물리주소를 그대로 포인터로
        // 캐스팅한다(Paging::init() 이전, 저지대 identity map 범위).
        const auto* archive = reinterpret_cast<const void*>(mod.physStart);
        const kernel::uint64_t archiveSize = mod.physEnd - mod.physStart;
        cpio::forEachEntry(archive, archiveSize, kLogCpioEntry, nullptr);
    }
}

void kLogMemoryMap(const kernel::HvmMemmapEntry* memmap, kernel::uint32_t count) {
    kernel::Serial::write("minicore: memory map (");
    kernel::Serial::writeHex(count);
    kernel::Serial::write(" entries)\n");
    for (kernel::uint32_t i = 0; i < count; ++i) {
        kernel::Serial::write("  base=");
        kernel::Serial::writeHex(memmap[i].addr);
        kernel::Serial::write(" size=");
        kernel::Serial::writeHex(memmap[i].size);
        kernel::Serial::write(" type=");
        kernel::Serial::writeHex(memmap[i].type);
        kernel::Serial::write("\n");
    }
}

// usable 영역들의 최대 끝 주소 - Paging::init()이 direct map으로 덮어야
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
// kLogCpioEntry가 이미 부팅 극초반에 채워 둠) 그 ELF를 파싱해 실제
// Process/UserThread로 ring3 진입시킨다(Process::execImage/kEnterRing3,
// PN-55D24891). PageFrameAllocator/Scheduler/AsyncReactor가 전부 준비된
// 뒤(=이 함수 호출 시점)에만 안전하다 - execImage가 유저 스택 페이지를
// 확보하고 UserThread::init()이 커널 스택을 확보하기 때문.
elf::Image gInitImage;
kernel::Process gInitProcess;
kernel::UserThread gInitThread;

void kSpawnInitProcess() {
    if (!gInitImageFound) {
        kernel::Serial::write("minicore: no \"init\" entry found in initrd modules - skipping first process spawn\n");
        return;
    }
    if (elf::Image::parse(gInitImageBuffer, gInitImageSize, &gInitImage) != elf::Error::None) {
        kernel::Serial::write("minicore: init image ELF parse FAILED\n");
        return;
    }
    if (!gInitProcess.init()) {
        kernel::Serial::write("minicore: init process address space allocation FAILED\n");
        return;
    }
    kernel::UserThread* thread = gInitProcess.execImage(gInitImage, &gInitThread);
    if (!thread) {
        kernel::Serial::write("minicore: init process execImage FAILED\n");
        return;
    }
    kernel::Scheduler::enqueue(kernel::Scheduler::currentCoreIndex(), thread);
    kernel::Serial::write("minicore: init process spawned, entry=");
    kernel::Serial::writeHex(gInitImage.entryPoint());
    kernel::Serial::write("\n");
}

}  // namespace

// boot.S가 higher-half로 넘어온 뒤 호출한다. rdi = 부팅 정보 구조체
// (PVH면 hvm_start_info, multiboot2면 그 정보 구조체)의 물리 주소,
// rsi = 어느 프로토콜인지(kBootProtocolPvh/kBootProtocolMultiboot2,
// EBX/EAX로 전달된 값을 boot.S가 저장해 뒀다가 넘김, PL-FC38956C).
// 이 함수 맨 위에서 프로토콜별로 memmap/rsdpPaddr를 같은 형태
// (HvmMemmapEntry 배열 + 물리주소)로 통일하고 나면, 그 뒤부터는 완전히
// 프로토콜 무관 공통 경로다. 이 시점에는 커널(ring 0)만 실행 중이다 -
// devmgr 등 "커널 서비스"는 아직 존재하지 않는다(SP-8B6B8D25 §2-A).
extern "C" void kMain(kernel::uint32_t startInfoAddr, kernel::uint32_t bootProtocol) {
    kernel::Serial::init();

    static kernel::HvmMemmapEntry gMb2MemmapBuffer[kernel::kMultiboot2MaxMemmapEntries];
    const kernel::HvmMemmapEntry* memmap = nullptr;
    kernel::uint32_t memmapEntries = 0;
    kernel::uint64_t rsdpPaddr = 0;
    kernel::uint64_t startInfoSize = 0;
    kernel::BootInfo bootInfo{};

    if (bootProtocol == kBootProtocolMultiboot2) {
        kernel::Serial::write("minicore: booted via multiboot2 (GRUB, higher-half, long mode)\n");
        kernel::uint32_t mb2TotalSize = 0;
        kernel::Multiboot2Info::parse(static_cast<kernel::uint64_t>(startInfoAddr), gMb2MemmapBuffer,
                                       kernel::kMultiboot2MaxMemmapEntries, &memmapEntries, &rsdpPaddr, &mb2TotalSize,
                                       &bootInfo);
        memmap = gMb2MemmapBuffer;
        startInfoSize = mb2TotalSize;
    } else {
        kernel::Serial::write("minicore: booted via Xen PVH (higher-half, long mode)\n");
        const auto* startInfo = reinterpret_cast<const kernel::HvmStartInfo*>(static_cast<kernel::uint64_t>(startInfoAddr));
        if (startInfo->magic == kernel::kHvmStartInfoMagic) {
            kernel::Serial::write("minicore: hvm_start_info magic OK\n");
        } else {
            kernel::Serial::write("minicore: hvm_start_info magic MISMATCH\n");
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
    kernel::Serial::write("minicore: GDT ready\n");

    kernel::Idt::init();
    kernel::Serial::write("minicore: IDT ready\n");

    kLogBootInfo(bootInfo);
    if (kCmdlineHasFlag(bootInfo.cmdline, "--disable-x2apic")) {
        kernel::Lapic::setX2ApicDisabled(true);
        kernel::Serial::write("minicore: --disable-x2apic requested, x2APIC will be forced off\n");
    }

    kLogMemoryMap(memmap, memmapEntries);

    // 순서 중요: Paging(direct map) -> Acpi(SRAT로 NUMA 토폴로지 확보,
    // direct map으로 테이블을 읽음) -> PageFrameAllocator(Acpi의 NUMA
    // 정보로 노드별 buddy 구성) -> Lapic(PageFrameAllocator에서 페이지
    // 테이블용 프레임을 받아옴 - 그 안에서 자기 자신의 id()를 부르지
    // 않도록 Lapic::isReady()로 방어돼 있음, 2026-09-14 실측으로
    // 발견한 초기화 순서 문제).
    kernel::Paging::init(kComputeMaxUsablePhysAddr(memmap, memmapEntries));
    kernel::Serial::write("minicore: direct physical map ready\n");

    if (kernel::Acpi::init(rsdpPaddr)) {
        kernel::Serial::write("minicore: ACPI MADT/SRAT parsed, cpu_count=");
        kernel::Serial::writeHex(kernel::Acpi::cpuCount());
        kernel::Serial::write(" numa_nodes=");
        kernel::Serial::writeHex(kernel::Acpi::numaNodeCount());
        kernel::Serial::write(" local_apic_addr=");
        kernel::Serial::writeHex(kernel::Acpi::localApicAddress());
        kernel::Serial::write(" ioapic_count=");
        kernel::Serial::writeHex(kernel::Acpi::ioApicCount());
        kernel::Serial::write(" hpet=");
        kernel::Serial::write(kernel::Acpi::hasHpet() ? "yes" : "no");
        kernel::Serial::write("\n");
        for (kernel::uint32_t i = 0; i < kernel::Acpi::cpuCount(); ++i) {
            kernel::Serial::write("  cpu[");
            kernel::Serial::writeHex(i);
            kernel::Serial::write("] apic_id=");
            kernel::Serial::writeHex(kernel::Acpi::cpuApicId(i));
            kernel::Serial::write(" numa_node=");
            kernel::Serial::writeHex(kernel::Acpi::cpuNumaNode(i));
            kernel::Serial::write("\n");
        }
        for (kernel::uint32_t i = 0; i < kernel::Acpi::ioApicCount(); ++i) {
            kernel::Serial::write("  ioapic[");
            kernel::Serial::writeHex(i);
            kernel::Serial::write("] id=");
            kernel::Serial::writeHex(kernel::Acpi::ioApicId(i));
            kernel::Serial::write(" addr=");
            kernel::Serial::writeHex(kernel::Acpi::ioApicAddress(i));
            kernel::Serial::write(" gsi_base=");
            kernel::Serial::writeHex(kernel::Acpi::ioApicGsiBase(i));
            kernel::Serial::write("\n");
        }
    } else {
        kernel::Serial::write("minicore: ACPI MADT parse FAILED\n");
    }

    // Acpi::cpuCount()만 있으면 되므로 여기서 바로 초기화한다(코어별
    // 큐를 만들어 두고, 실제 디스패치는 각 코어가 Scheduler::runLoop()
    // 에 들어가면서 시작된다 - PL-2D3184BC 4/5/6단계).
    kernel::Scheduler::init();

    kernel::PageFrameAllocator::init(
        memmap, memmapEntries,
        reinterpret_cast<kernel::uint64_t>(kernel_phys_start),
        reinterpret_cast<kernel::uint64_t>(kernel_phys_end),
        static_cast<kernel::uint64_t>(startInfoAddr), startInfoSize);

    kernel::Serial::write("minicore: page frame allocator ready, nodes=");
    kernel::Serial::writeHex(kernel::PageFrameAllocator::numaNodeCount());
    kernel::Serial::write(" free_pages=");
    kernel::Serial::writeHex(kernel::PageFrameAllocator::freePageCount());
    kernel::Serial::write("\n");

    // PageFrameAllocator 이후, Scheduler::currentCoreIndex()/
    // PreemptionGuard를 실제로 쓰는 첫 alloc()/free() 호출(Lapic::init()
    // 이후) 전이면 아무때나 무방하다 - init() 자체는 정적 구조만
    // 채운다(SP-D7013B26).
    kernel::GenericSlabAllocator::init();
    kernel::Serial::write("minicore: slab allocator ready (libkmm)\n");

    kernel::Lapic::init();
    kernel::Serial::write("minicore: LAPIC ready, mode=");
    kernel::Serial::write(kernel::Lapic::usesX2Apic() ? "x2apic" : "xapic");
    kernel::Serial::write(" id=");
    kernel::Serial::writeHex(kernel::Lapic::id());
    kernel::Serial::write("\n");

    // Acpi::cpuApicId()/Lapic::id()로 자기 코어 인덱스를 찾아야 해서
    // 반드시 이 둘 이후에 호출해야 한다(gdt.h 참고 - IST1을 #DF
    // 전용으로 채우고 ltr).
    kernel::Gdt::loadTssForThisCore();
    kernel::Serial::write("minicore: TSS/IST ready (core 0)\n");

    kernel::Scheduler::startTickOnThisCore();
    kernel::Serial::write("minicore: scheduler tick ready (LAPIC, ");
    kernel::Serial::writeHex(kernel::kSchedulerTickHz);
    kernel::Serial::write("Hz, vector=");
    kernel::Serial::writeHex(kernel::kSchedulerTickVector);
    kernel::Serial::write(")\n");

    kernel::AsyncReactor::initForThisCore();
    kernel::Serial::write("minicore: async reactor ready (core 0)\n");

    // 전역 테이블 하나뿐이라 BSP에서 딱 한 번만 - AP(kApMain)는 이걸
    // 다시 부르지 않는다(SyscallRegistry::registerHandler가 이미 쓰인
    // 슬롯에 재등록을 거부하므로 안전장치는 있지만, 애초에 호출
    // 자체를 한 곳에만 둔다).
    kernel::Channel::registerSyscallEndpoints();
    kernel::Serial::write("minicore: channel IPC syscall endpoints registered\n");

    // 전역 IDT 등록이라 BSP에서 한 번만(위 registerSyscallEndpoints와
    // 같은 이유).
    kernel::TlbShootdown::init();
    kernel::Serial::write("minicore: TLB shootdown IPI handler registered\n");

    // KernelAddressSpaceManager(SP-2AAD7C8D §2, PN-012E8C1A) - 위
    // TlbShootdown::init() 이후에만 안전(unmapRegion()이 broadcast()를
    // 부름). 전역 싱글턴이라 BSP에서 한 번만.
    kernel::KernelAddressSpaceManager::init();
    kernel::Serial::write("minicore: kernel address space manager ready\n");

    kernel::IoApic::init();
    kernel::Serial::write("minicore: IOAPIC mapped\n");

    kernel::Timer::init();
    kernel::Serial::write("minicore: timer ready (100Hz), source=");
    kernel::Serial::write(kernel::Timer::usesHpet() ? "hpet" : "lapic+pit");
    kernel::Serial::write(", enabling interrupts\n");

    kernel::Pci::init();
    kernel::Serial::write("minicore: PCI config access=");
    kernel::Serial::write(kernel::Pci::usesMmconfig() ? "mmconfig+legacy" : "legacy");
    kernel::Serial::write("\nminicore: PCI enumeration:\n");
    kernel::Pci::enumerate(kLogPciDevice);

    kSpawnInitProcess();

    // 반드시 sti 이후에 호출해야 한다(SMP AP 기동도 마찬가지 이유).
    asm volatile("sti");

    kernel::Smp::startApCores();

    // 이 지점부터 BSP 자신도 스케줄러 디스패치 루프에 들어간다 -
    // 절대 반환하지 않는다(PL-2D3184BC 5/6단계).
    kernel::Scheduler::runLoop();
}
