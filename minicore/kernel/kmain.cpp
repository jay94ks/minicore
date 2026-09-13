#include "acpi.h"
#include "hvm_start_info.h"
#include "idt.h"
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
constexpr unsigned int kBootProtocolMultiboot2 = 1;

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

void kLogMemoryMap(const kernel::HvmMemmapEntry* memmap, unsigned int count) {
    kernel::Serial::write("minicore: memory map (");
    kernel::Serial::writeHex(count);
    kernel::Serial::write(" entries)\n");
    for (unsigned int i = 0; i < count; ++i) {
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
extern "C" void kMain(unsigned int startInfoAddr, unsigned int bootProtocol) {
    kernel::Serial::init();

    static kernel::HvmMemmapEntry gMb2MemmapBuffer[kernel::kMultiboot2MaxMemmapEntries];
    const kernel::HvmMemmapEntry* memmap = nullptr;
    unsigned int memmapEntries = 0;
    unsigned long rsdpPaddr = 0;
    unsigned long startInfoSize = 0;

    if (bootProtocol == kBootProtocolMultiboot2) {
        kernel::Serial::write("minicore: booted via multiboot2 (GRUB, higher-half, long mode)\n");
        unsigned int mb2TotalSize = 0;
        kernel::Multiboot2Info::parse(static_cast<unsigned long>(startInfoAddr), gMb2MemmapBuffer,
                                       kernel::kMultiboot2MaxMemmapEntries, &memmapEntries, &rsdpPaddr, &mb2TotalSize);
        memmap = gMb2MemmapBuffer;
        startInfoSize = mb2TotalSize;
    } else {
        kernel::Serial::write("minicore: booted via Xen PVH (higher-half, long mode)\n");
        const auto* startInfo = reinterpret_cast<const kernel::HvmStartInfo*>(static_cast<unsigned long>(startInfoAddr));
        if (startInfo->magic == kernel::kHvmStartInfoMagic) {
            kernel::Serial::write("minicore: hvm_start_info magic OK\n");
        } else {
            kernel::Serial::write("minicore: hvm_start_info magic MISMATCH\n");
        }
        memmap = reinterpret_cast<const kernel::HvmMemmapEntry*>(startInfo->memmapPaddr);
        memmapEntries = startInfo->memmapEntries;
        rsdpPaddr = startInfo->rsdpPaddr;
        startInfoSize = sizeof(kernel::HvmStartInfo);
    }

    kernel::Idt::init();
    kernel::Serial::write("minicore: IDT ready\n");

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
        kernel::Serial::write(" ioapic_addr=");
        kernel::Serial::writeHex(kernel::Acpi::ioApicAddress());
        kernel::Serial::write(" hpet=");
        kernel::Serial::write(kernel::Acpi::hasHpet() ? "yes" : "no");
        kernel::Serial::write("\n");
        for (unsigned int i = 0; i < kernel::Acpi::cpuCount(); ++i) {
            kernel::Serial::write("  cpu[");
            kernel::Serial::writeHex(i);
            kernel::Serial::write("] apic_id=");
            kernel::Serial::writeHex(kernel::Acpi::cpuApicId(i));
            kernel::Serial::write(" numa_node=");
            kernel::Serial::writeHex(kernel::Acpi::cpuNumaNode(i));
            kernel::Serial::write("\n");
        }
    } else {
        kernel::Serial::write("minicore: ACPI MADT parse FAILED\n");
    }

    kernel::PageFrameAllocator::init(
        memmap, memmapEntries,
        reinterpret_cast<unsigned long>(kernel_phys_start),
        reinterpret_cast<unsigned long>(kernel_phys_end),
        static_cast<unsigned long>(startInfoAddr), startInfoSize);

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

    kernel::Serial::write("minicore: PCI enumeration:\n");
    kernel::Pci::enumerate(kLogPciDevice);

    // SMP AP 기동(PL-65C20380)은 Timer 틱 기반 타임아웃 대기를 쓰므로
    // 반드시 sti 이후에 호출해야 한다.
    asm volatile("sti");

    kernel::Smp::startApCores();

    for (;;) {
        asm volatile("hlt");
    }
}
