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

// kPhysToVirt의 역변환 - direct map 안의 가상주소에만 유효하다(그
// 밖의 임의 가상주소를 넘기면 안 됨, 호출부 책임). GenericSlabAllocator
// (SP-D7013B26)가 2048B 초과 요청을 PageFrameAllocator로 직접 위임할
// 때, free() 시점에 되돌려줄 물리주소를 구하는 데 쓴다.
inline uint64_t kVirtToPhys(uint64_t virtAddr) {
    return virtAddr - kDirectMapBase;
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
    // pml4Phys를 생략(0)하면 현재 CR3(지금 실행 중인 주소공간)를 쓴다 -
    // 0을 넘겨서 진짜 물리주소 0번 프레임(PML4용으로 쓸 리 없는 값,
    // 부트로더가 커널 이미지를 얹어 둔 자리라 항상 예약됨)을 가리킬
    // 일은 없다. 0이 아닌 값을 넘기면 **아직 CR3에 설치되지 않은**
    // 다른 주소공간(예: 새로 만드는 프로세스, Process::init 참고)을
    // direct map을 통해 직접 구성한다 - CR3을 매번 전환하지 않아도
    // 되므로 구성 도중 인터럽트가 끼어들어도 지금 실행 중인 주소공간을
    // 전혀 건드리지 않아 더 안전하다(SP-8B6B8D25 §5).
    static void mapPage(uint64_t virtualAddr, uint64_t physicalAddr, uint64_t flags, uint64_t pml4Phys = 0);

    // 매핑을 해제한다(TLB도 무효화) - 매핑돼 있지 않으면 아무 일도
    // 안 한다. pml4Phys 의미는 mapPage와 동일(생략 시 현재 CR3).
    static void unmapPage(uint64_t virtualAddr, uint64_t pml4Phys = 0);

    // #PF(vector 14) 핸들러가 호출한다(idt.cpp). faultAddr는 CR2,
    // errorCode는 하드웨어가 스택에 남긴 값 그대로. 이 폴트를 정말
    // 처리했으면(=매핑을 새로 붙여서 재실행하면 될 상황) true를
    // 반환한다 - false면 호출부가 평소대로 패닉한다. kLazyZoneBase
    // 범위 안의 not-present 폴트만 처리한다(권한 위반은 그대로
    // 패닉시킨다 - 조용히 덮어쓰지 않는다).
    static bool handlePageFault(uint64_t faultAddr, uint64_t errorCode);

    // 지금 실행 중인 CR3(활성 PML4의 물리 프레임 주소) - 새 주소공간을
    // 만들 때 "커널 상위 절반"을 복사해 올 원본으로 쓴다
    // (createAddressSpace 참고). 진단/장래 재사용 목적으로도 공개.
    static uint64_t currentPml4Phys();

    // 새 프로세스용 PML4 프레임을 하나 확보해 0으로 초기화한 뒤,
    // 커널이 사는 상위 절반(canonical higher half - PML4 인덱스
    // 256~511, kDirectMapBase=0xFFFF800000000000이 정확히 그 경계라
    // direct map/지연 매핑 구역/커널 이미지 전부 이 범위 안에 있다)만
    // 현재 PML4에서 그대로 복사한다 - **엔트리 값만 복사**하므로 실제
    // 하위 테이블(PDPT 이하)은 모든 프로세스가 물리적으로 공유한다
    // (표준적인 "커널은 모든 주소공간에서 항상 같다" 기법 - Process가
    // 소멸돼도 이 공유 테이블은 절대 반납하면 안 된다, 하위 절반만
    // 프로세스 소유). 실패(PageFrameAllocator 고갈) 시 0.
    static uint64_t createAddressSpace();

    // createAddressSpace()가 만든 PML4를 반납한다 - **하위 절반
    // (유저 공간, PML4 인덱스 0~255)에 실제로 매핑된 페이지가 이미
    // 전부 해제(unmapPage)돼 있어야 한다**는 게 호출부 책임이다(이
    // 함수 자신은 PML4 프레임 자체만 반납하고, 그 안의 하위 절반
    // 엔트리가 가리키는 PDPT 이하는 건드리지 않는다 - 프로세스 자원
    // 정리 순서 자체는 아직 이 프로젝트에 확정돼 있지 않음, PN-40E976F2
    // 참고).
    static void destroyAddressSpace(uint64_t pml4Phys);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PAGING_H
