#ifndef MINICORE_KERNEL_PAGE_FRAME_ALLOCATOR_H
#define MINICORE_KERNEL_PAGE_FRAME_ALLOCATOR_H

#include "hvm_start_info.h"
#include "libkenv/types.h"

namespace kernel {

constexpr uint32_t kPfaMaxNumaNodes = 8;  // Acpi::kAcpiMaxNumaNodes와 맞춘 상한

// 물리 페이지 프레임 buddy 할당자 (DS-D4E5C451 - "페이지 단위 +
// buddy allocator", NUMA 노드별로 실제로 분리 - 2026-09-14 설계자
// 지시 QU-88936C2B). Acpi::init()(SRAT 파싱 포함) 이후에 호출해야
// 한다 - 노드별로 어떤 물리 범위가 속하는지 SRAT 메모리 어피니티로
// 판정한다(SRAT가 없으면 전부 노드 0). 커널이 정적으로 identity
// map해 둔 저지대 물리 메모리(현재 1GiB, boot.S 참고) 안에서만
// 관리한다 - 그 밖의 RAM은 아직 이 v1의 범위 밖이다.
class PageFrameAllocator {
public:
    // memmap: HvmStartInfo::memmapPaddr가 가리키는 배열(entryCount개).
    // kernelPhysStart/End: 커널 이미지 자신의 물리 범위(겹치는 usable
    // 영역에서 제외) - 링커 심볼로 구한다.
    static void init(const HvmMemmapEntry* memmap, uint32_t entryCount,
                      uint64_t kernelPhysStart, uint64_t kernelPhysEnd,
                      uint64_t startInfoAddr, uint64_t startInfoSize);

    // 4KiB 페이지 하나 - 실패하면 0을 반환한다(널 페이지는 항상 예약됨).
    // allocPage()는 지금 실행 중인 코어(Lapic::id())가 속한 노드에서
    // 우선 할당하고, 그 노드가 바닥나면 다른 노드로 넘어간다.
    static uint64_t allocPage();
    static uint64_t allocPageOnNode(uint32_t node);
    // 어느 노드 소속인지는 주소로 스스로 판별한다 - 호출부가 기억할
    // 필요 없다.
    static void freePage(uint64_t physAddr);

    // order: 2^order 페이지(4KiB << order) 블록.
    static uint64_t allocOrder(uint32_t order);
    static uint64_t allocOrderOnNode(uint32_t node, uint32_t order);
    static void freeOrder(uint64_t physAddr, uint32_t order);

    static uint64_t freePageCount();  // 전체 노드 합
    static uint32_t numaNodeCount();
    static uint64_t freePageCountOnNode(uint32_t node);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PAGE_FRAME_ALLOCATOR_H
