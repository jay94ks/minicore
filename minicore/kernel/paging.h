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
// 실제 설치된 usable 메모리를 전부 덮도록 1GiB 페이지로 동적으로
// 매핑한다(Paging::init(maxPhysAddr) 참고, PN-4AA5425D - "설계 변경
// 불필요, 순수 확장" 확정) - 최소 4GiB(LAPIC/IOAPIC/HPET 등 저지대
// MMIO가 항상 이 안에 있음)는 항상 보장하고, 최대 512GiB(PDPT 하나가
// 가질 수 있는 엔트리 상한)까지 늘어난다. 512GiB를 넘는 메모리는 여전히
// v1 범위 밖(PDPT를 여러 개 두는 구조 변경이 필요 - 후속 과제).
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
    // maxPhysAddr: 메모리 맵에서 찾은 usable 영역의 최대 끝 주소(호출부
    // -kmain.cpp-가 PageFrameAllocator::init()과 같은 memmap을 스캔해
    // 구한다) - 이 값까지 1GiB 페이지로 direct map을 늘린다(최소
    // 4GiB/최대 512GiB로 clamp, kDirectMapBase 주석 참고).
    static void init(uint64_t maxPhysAddr);

    // init()이 실제로 확보한 direct map의 범위(바이트, 위 clamp 적용
    // 후의 값) - PageFrameAllocator::init()이 이 값을 넘는 usable
    // 영역을 프레임 풀에서 잘라내는 데 쓴다(그 이상은 direct map으로
    // 볼 수 없는 물리 프레임이라 애초에 내줄 수 없음). init() 이전에
    // 부르면 0.
    static uint64_t directMapLimit();

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

    // [PL-57CF86EF 병합 경로 (a): 매핑 시점 즉시 대형 페이지] virtualAddr/
    // physicalAddr/sizeBytes로 지정된 범위를 매핑한다 - 결과(각 4KiB
    // 주소가 가리키는 물리 프레임)는 mapPage()를 sizeBytes/4096번
    // 반복 호출하는 것과 완전히 동일하지만, 2MiB로 정렬된(가상/물리
    // 둘 다) 부분 구간이면서 그 PD 슬롯이 아직 완전히 비어 있는
    // 경우에 한해 그 구간만 PD 레벨 PS 비트로 즉시 2MiB 페이지 하나로
    // 매핑한다(페이지 테이블 엔트리 개수만 줄어듦 - 이미 그 자리에
    // 뭔가 매핑돼 있으면 기존 내용을 잃어버리지 않도록 안전하게 4KiB
    // 단위로 물러난다). 나머지(정렬에서 벗어난 자투리, 이미 뭔가
    // 있는 슬롯)는 그대로 mapPage()로 4KiB씩 처리한다.
    static void mapRange(uint64_t virtualAddr, uint64_t physicalAddr, uint64_t sizeBytes, uint64_t flags, uint64_t pml4Phys = 0);

    // [PL-57CF86EF 병합 경로 (c): 명시적 API 호출] [virtualAddr,
    // virtualAddr+sizeBytes) 범위를 2MiB 정렬 구간 단위로 훑어, 이미
    // 4KiB 단위로 매핑돼 있으면서 병합 불변 조건(512개 엔트리 전부
    // present + 물리주소 연속 + PAGE_WRITABLE/PAGE_USER/
    // PAGE_CACHE_DISABLE 전부 동일)을 만족하는 구간을 그 자리에서
    // 2MiB PS 엔트리로 합친다 - 남는 PT 프레임만 반납하고(가리키던
    // 물리 리프 페이지는 그대로 유지) 실제 매핑 내용은 바뀌지 않는다.
    // 조건을 만족하지 않는 구간(재배치가 필요한 경우 - 병합 경로 (b)
    // 몫)은 건드리지 않고 그대로 둔다. 하나 이상 병합했으면 true.
    static bool mergeRange(uint64_t virtualAddr, uint64_t sizeBytes, uint64_t pml4Phys = 0);

    // 매핑을 해제한다(TLB도 무효화) - 매핑돼 있지 않으면 아무 일도
    // 안 한다. pml4Phys 의미는 mapPage와 동일(생략 시 현재 CR3).
    static void unmapPage(uint64_t virtualAddr, uint64_t pml4Phys = 0);

    // virtualAddr이 매핑된 물리 주소(4KiB 정렬)를 반환한다 - 매핑돼
    // 있지 않으면 0. unmapPage()는 매핑을 지우기만 하고 그 전에 물리
    // 프레임이 무엇이었는지 알려주지 않는데(#PF 온디맨드 매핑 경로엔
    // 필요 없었음), VMA 관리자(SP-2AAD7C8D §2, ProcessAddressSpaceManager/
    // KernelAddressSpaceManager)가 unmap 시 그 물리 프레임을
    // PageFrameAllocator에 실제로 반납하려면 unmapPage 호출 전에 먼저
    // 이 함수로 읽어 둬야 한다.
    static uint64_t translatePage(uint64_t virtualAddr, uint64_t pml4Phys = 0);

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
    // (유저 공간, PML4 인덱스 0~255)에 실제로 매핑된 leaf 데이터
    // 페이지가 이미 전부 해제(unmapPage)돼 있어야 한다**는 게 호출부
    // 책임이다. 이 함수 자신은 그 위에서 하위 절반의 PDPT/PD/PT
    // 중간 테이블 프레임을 재귀적으로 찾아 반납한 뒤 PML4 프레임까지
    // 반납한다(PN-2E6CB2D5, QU-A2348197 설계자 확정안 (b) - 종료
    // 시점 일괄 재귀 순회). 상위 절반(256~511, 커널 공유 테이블)은
    // 절대 건드리지 않는다 - createAddressSpace()가 엔트리만 복사해
    // 모든 프로세스가 물리적으로 공유하는 테이블이라 잘못 반납하면
    // 전체 시스템이 깨진다.
    static void destroyAddressSpace(uint64_t pml4Phys);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PAGING_H
