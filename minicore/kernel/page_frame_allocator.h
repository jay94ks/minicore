#ifndef MINICORE_KERNEL_PAGE_FRAME_ALLOCATOR_H
#define MINICORE_KERNEL_PAGE_FRAME_ALLOCATOR_H

#include "hvm_start_info.h"

namespace kernel {

// 물리 페이지 프레임 buddy 할당자 (DS-D4E5C451 - "페이지 단위 +
// buddy allocator"). 지금은 단일 NUMA 노드로만 동작한다 - 멀티노드
// 확장 범위는 QU-88936C2B로 확인 대기 중(SP-8B6B8D25 미결). 커널이
// 정적으로 identity map해 둔 저지대 물리 메모리(현재 1GiB, boot.S
// 참고) 안에서만 관리한다 - 그 밖의 RAM은 아직 이 v1의 범위 밖이다.
class PageFrameAllocator {
public:
    // memmap: HvmStartInfo::memmapPaddr가 가리키는 배열(entryCount개).
    // kernelPhysStart/End: 커널 이미지 자신의 물리 범위(겹치는 usable
    // 영역에서 제외) - 링커 심볼로 구한다.
    static void kInit(const HvmMemmapEntry* memmap, unsigned int entryCount,
                       unsigned long kernelPhysStart, unsigned long kernelPhysEnd,
                       unsigned long startInfoAddr, unsigned long startInfoSize);

    // 4KiB 페이지 하나 - 실패하면 0을 반환한다(널 페이지는 항상 예약됨).
    static unsigned long kAllocPage();
    static void kFreePage(unsigned long physAddr);

    // order: 2^order 페이지(4KiB << order) 블록.
    static unsigned long kAllocOrder(unsigned int order);
    static void kFreeOrder(unsigned long physAddr, unsigned int order);

    static unsigned long kFreePageCount();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PAGE_FRAME_ALLOCATOR_H
