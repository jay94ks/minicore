#include "hvm_start_info.h"
#include "idt.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "serial.h"

namespace {

// linker.ld가 정의하는 커널 자신의 물리 범위 - usable 메모리에서
// 제외하는 데 쓴다(page_frame_allocator.cpp).
extern "C" char kernel_phys_start[];
extern "C" char kernel_phys_end[];

void kLogMemoryMap(const kernel::HvmMemmapEntry* memmap, unsigned int count) {
    kernel::Serial::kWrite("minicore: memory map (");
    kernel::Serial::kWriteHex(count);
    kernel::Serial::kWrite(" entries)\n");
    for (unsigned int i = 0; i < count; ++i) {
        kernel::Serial::kWrite("  base=");
        kernel::Serial::kWriteHex(memmap[i].addr);
        kernel::Serial::kWrite(" size=");
        kernel::Serial::kWriteHex(memmap[i].size);
        kernel::Serial::kWrite(" type=");
        kernel::Serial::kWriteHex(memmap[i].type);
        kernel::Serial::kWrite("\n");
    }
}

}  // namespace

// boot.S가 higher-half로 넘어온 뒤 호출한다. rdi = struct
// hvm_start_info의 물리 주소(PVH direct boot ABI, EBX로 전달된 값을
// boot.S가 그대로 넘김). 이 시점에는 커널(ring 0)만 실행 중이다 -
// devmgr 등 "커널 서비스"는 아직 존재하지 않는다(SP-8B6B8D25 §2-A).
extern "C" void kMain(unsigned int startInfoAddr) {
    kernel::Serial::kInit();
    kernel::Serial::kWrite("minicore: booted via Xen PVH (higher-half, long mode)\n");

    const auto* startInfo = reinterpret_cast<const kernel::HvmStartInfo*>(static_cast<unsigned long>(startInfoAddr));
    if (startInfo->magic == kernel::kHvmStartInfoMagic) {
        kernel::Serial::kWrite("minicore: hvm_start_info magic OK\n");
    } else {
        kernel::Serial::kWrite("minicore: hvm_start_info magic MISMATCH\n");
    }

    kernel::Idt::kInit();
    kernel::Serial::kWrite("minicore: IDT ready\n");

    const auto* memmap = reinterpret_cast<const kernel::HvmMemmapEntry*>(startInfo->memmapPaddr);
    kLogMemoryMap(memmap, startInfo->memmapEntries);

    kernel::PageFrameAllocator::kInit(
        memmap, startInfo->memmapEntries,
        reinterpret_cast<unsigned long>(kernel_phys_start),
        reinterpret_cast<unsigned long>(kernel_phys_end),
        static_cast<unsigned long>(startInfoAddr), sizeof(kernel::HvmStartInfo));

    kernel::Serial::kWrite("minicore: page frame allocator ready, free pages=");
    kernel::Serial::kWriteHex(kernel::PageFrameAllocator::kFreePageCount());
    kernel::Serial::kWrite("\n");

    kernel::Paging::kInit();
    kernel::Serial::kWrite("minicore: direct physical map ready\n");

    for (;;) {
        asm volatile("hlt");
    }
}
