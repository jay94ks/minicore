#include "acpi.h"
#include "boot_info.h"
#include "hvm_start_info.h"
#include "idt.h"
#include "libcpio/cpio.h"
#include "libkenv/types.h"
#include "ioapic.h"
#include "lapic.h"
#include "multiboot2.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "pci.h"
#include "serial.h"
#include "smp.h"
#include "timer.h"

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

// 부팅 모듈(initrd)이 있으면 libcpio로 훑어 로그를 남긴다(QU-9DCDCE3E
// - "initrd 역시도 마찬가지다"). 아직 이 CPIO 내용을 실제로 마운트할
// 파일시스템/디바이스 관리자가 없어 지금은 진단 로그까지만 한다 -
// 실제 사용(파일 열람 등)은 그 서브시스템이 생길 때 이 파서를 그대로
// 재사용하면 된다.
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
    kernel::Paging::init();
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

    kernel::Lapic::init();
    kernel::Serial::write("minicore: LAPIC ready, mode=");
    kernel::Serial::write(kernel::Lapic::usesX2Apic() ? "x2apic" : "xapic");
    kernel::Serial::write(" id=");
    kernel::Serial::writeHex(kernel::Lapic::id());
    kernel::Serial::write("\n");

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

    // 반드시 sti 이후에 호출해야 한다(SMP AP 기동도 마찬가지 이유).
    asm volatile("sti");

    kernel::Smp::startApCores();

    for (;;) {
        asm volatile("hlt");
    }
}
