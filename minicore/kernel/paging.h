#ifndef MINICORE_KERNEL_PAGING_H
#define MINICORE_KERNEL_PAGING_H

#include "libkenv/types.h"

namespace kernel {

constexpr uint64_t PAGE_PRESENT = 1UL << 0;
constexpr uint64_t PAGE_WRITABLE = 1UL << 1;
constexpr uint64_t PAGE_USER = 1UL << 2;
constexpr uint64_t PAGE_CACHE_DISABLE = 1UL << 4;  // MMIO(LAPIC 등)는 반드시 이걸 켜야 한다

// 커널이 임의 물리 프레임을 한 번에 볼 수 있게 만드는 direct physical
// map(가상 kDirectMapBase + 물리주소 = 그 물리 프레임)의 시작 주소.
// 첫 4GiB를 1GiB 페이지 4개로 매핑해 둔다(Paging::init) - 그 이상은
// 아직 v1 범위 밖(관련 결정: DS-D4E5C451, 후속 DC 예정).
constexpr uint64_t kDirectMapBase = 0xFFFF800000000000UL;

inline uint64_t kPhysToVirt(uint64_t physAddr) {
    return kDirectMapBase + physAddr;
}

// 아직 실제 유저/커널 주소공간 서술자(VMA)가 없어서, 온디맨드 매핑을
// 시험할 "지연 매핑 구역"을 하나 고정으로 둔다 - 이 범위 안에서
// not-present 폴트가 나면 프레임을 새로 붙여준다. 나중에 진짜 힙/
// 프로세스 주소공간이 생기면 이 구역이 그 정책의 첫 사용처가 될 수
// 있다(지금은 자리표시자).
constexpr uint64_t kLazyZoneBase = 0xFFFF900000000000UL;
constexpr uint64_t kLazyZoneSize = 0x40000000UL;  // 1GiB

// 온디맨드 가상 메모리 관리 (SP-8B6B8D25 §5) - PageFrameAllocator가
// 관리하는 물리 프레임을 실제 페이지 테이블(PML4/PDPT/PD/PT)에
// 매핑/해제한다. 새 중간 테이블이 필요하면 PageFrameAllocator에서
// 프레임을 받아온다 - 그 프레임은 항상 정적으로 identity map된 저지대
// 1GiB 안이라(현재 한도) 물리 주소를 그대로 포인터로 써서 초기화할
// 수 있다.
class Paging {
public:
    // direct physical map을 구성한다 - mapPage/kUnmapPage보다 먼저
    // 호출해야 한다(둘 다 CR3을 그대로 쓰긴 하지만, direct map 없이도
    // 동작은 함 - 다만 커널이 임의 물리 주소를 볼 방법이 없어진다).
    static void init();

    // virtualAddr을 physicalAddr(4KiB 정렬)에 매핑한다. 필요한 중간
    // 테이블은 그때그때 만든다. 이미 매핑돼 있으면 덮어쓴다.
    static void mapPage(uint64_t virtualAddr, uint64_t physicalAddr, uint64_t flags);

    // 매핑을 해제한다(TLB도 무효화) - 매핑돼 있지 않으면 아무 일도
    // 안 한다.
    static void unmapPage(uint64_t virtualAddr);

    // #PF(vector 14) 핸들러가 호출한다(idt.cpp). faultAddr는 CR2,
    // errorCode는 하드웨어가 스택에 남긴 값 그대로. 이 폴트를 정말
    // 처리했으면(=매핑을 새로 붙여서 재실행하면 될 상황) true를
    // 반환한다 - false면 호출부가 평소대로 패닉한다. kLazyZoneBase
    // 범위 안의 not-present 폴트만 처리한다(권한 위반은 그대로
    // 패닉시킨다 - 조용히 덮어쓰지 않는다).
    static bool handlePageFault(uint64_t faultAddr, uint64_t errorCode);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PAGING_H
