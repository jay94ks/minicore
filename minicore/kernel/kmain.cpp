#include "acpi.h"
#include "hvm_start_info.h"
#include "idt.h"
#include "lapic.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "serial.h"
#include "timer.h"

namespace {

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

// boot.S가 higher-half로 넘어온 뒤 호출한다. rdi = struct
// hvm_start_info의 물리 주소(PVH direct boot ABI, EBX로 전달된 값을
// boot.S가 그대로 넘김). 이 시점에는 커널(ring 0)만 실행 중이다 -
// devmgr 등 "커널 서비스"는 아직 존재하지 않는다(SP-8B6B8D25 §2-A).
extern "C" void kMain(unsigned int startInfoAddr) {
    kernel::Serial::init();
    kernel::Serial::write("minicore: booted via Xen PVH (higher-half, long mode)\n");

    const auto* startInfo = reinterpret_cast<const kernel::HvmStartInfo*>(static_cast<unsigned long>(startInfoAddr));
    if (startInfo->magic == kernel::kHvmStartInfoMagic) {
        kernel::Serial::write("minicore: hvm_start_info magic OK\n");
    } else {
        kernel::Serial::write("minicore: hvm_start_info magic MISMATCH\n");
    }

    kernel::Idt::init();
    kernel::Serial::write("minicore: IDT ready\n");

    const auto* memmap = reinterpret_cast<const kernel::HvmMemmapEntry*>(startInfo->memmapPaddr);
    kLogMemoryMap(memmap, startInfo->memmapEntries);

    // 순서 중요: Paging(direct map) -> Acpi(SRAT로 NUMA 토폴로지 확보,
    // direct map으로 테이블을 읽음) -> PageFrameAllocator(Acpi의 NUMA
    // 정보로 노드별 buddy 구성) -> Lapic(PageFrameAllocator에서 페이지
    // 테이블용 프레임을 받아옴 - 그 안에서 자기 자신의 id()를 부르지
    // 않도록 Lapic::isReady()로 방어돼 있음, 2026-09-14 실측으로
    // 발견한 초기화 순서 문제).
    kernel::Paging::init();
    kernel::Serial::write("minicore: direct physical map ready\n");

    if (kernel::Acpi::init(startInfo->rsdpPaddr)) {
        kernel::Serial::write("minicore: ACPI MADT/SRAT parsed, cpu_count=");
        kernel::Serial::writeHex(kernel::Acpi::cpuCount());
        kernel::Serial::write(" numa_nodes=");
        kernel::Serial::writeHex(kernel::Acpi::numaNodeCount());
        kernel::Serial::write(" local_apic_addr=");
        kernel::Serial::writeHex(kernel::Acpi::localApicAddress());
        kernel::Serial::write(" ioapic_addr=");
        kernel::Serial::writeHex(kernel::Acpi::ioApicAddress());
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
        memmap, startInfo->memmapEntries,
        reinterpret_cast<unsigned long>(kernel_phys_start),
        reinterpret_cast<unsigned long>(kernel_phys_end),
        static_cast<unsigned long>(startInfoAddr), sizeof(kernel::HvmStartInfo));

    kernel::Serial::write("minicore: page frame allocator ready, nodes=");
    kernel::Serial::writeHex(kernel::PageFrameAllocator::numaNodeCount());
    kernel::Serial::write(" free_pages=");
    kernel::Serial::writeHex(kernel::PageFrameAllocator::freePageCount());
    kernel::Serial::write("\n");

    kernel::Lapic::init();
    kernel::Serial::write("minicore: LAPIC ready, id=");
    kernel::Serial::writeHex(kernel::Lapic::id());
    kernel::Serial::write("\n");

    kernel::Timer::init();
    kernel::Serial::write("minicore: timer calibrated (100Hz), enabling interrupts\n");

    asm volatile("sti");

    for (;;) {
        asm volatile("hlt");
    }
}
