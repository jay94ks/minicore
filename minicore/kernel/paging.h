#ifndef MINICORE_KERNEL_PAGING_H
#define MINICORE_KERNEL_PAGING_H

namespace kernel {

constexpr unsigned long PAGE_PRESENT = 1UL << 0;
constexpr unsigned long PAGE_WRITABLE = 1UL << 1;
constexpr unsigned long PAGE_USER = 1UL << 2;

// 커널이 임의 물리 프레임을 한 번에 볼 수 있게 만드는 direct physical
// map(가상 kDirectMapBase + 물리주소 = 그 물리 프레임)의 시작 주소.
// 첫 4GiB를 1GiB 페이지 4개로 매핑해 둔다(Paging::kInit) - 그 이상은
// 아직 v1 범위 밖(관련 결정: DS-D4E5C451, 후속 DC 예정).
constexpr unsigned long kDirectMapBase = 0xFFFF800000000000UL;

inline unsigned long kPhysToVirt(unsigned long physAddr) {
    return kDirectMapBase + physAddr;
}

// 온디맨드 가상 메모리 관리 (SP-8B6B8D25 §5) - PageFrameAllocator가
// 관리하는 물리 프레임을 실제 페이지 테이블(PML4/PDPT/PD/PT)에
// 매핑/해제한다. 새 중간 테이블이 필요하면 PageFrameAllocator에서
// 프레임을 받아온다 - 그 프레임은 항상 정적으로 identity map된 저지대
// 1GiB 안이라(현재 한도) 물리 주소를 그대로 포인터로 써서 초기화할
// 수 있다.
class Paging {
public:
    // direct physical map을 구성한다 - kMapPage/kUnmapPage보다 먼저
    // 호출해야 한다(둘 다 CR3을 그대로 쓰긴 하지만, direct map 없이도
    // 동작은 함 - 다만 커널이 임의 물리 주소를 볼 방법이 없어진다).
    static void kInit();

    // virtualAddr을 physicalAddr(4KiB 정렬)에 매핑한다. 필요한 중간
    // 테이블은 그때그때 만든다. 이미 매핑돼 있으면 덮어쓴다.
    static void kMapPage(unsigned long virtualAddr, unsigned long physicalAddr, unsigned long flags);

    // 매핑을 해제한다(TLB도 무효화) - 매핑돼 있지 않으면 아무 일도
    // 안 한다.
    static void kUnmapPage(unsigned long virtualAddr);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PAGING_H
