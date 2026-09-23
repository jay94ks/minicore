#ifndef MINICORE_KERNEL_PAGING_H
#define MINICORE_KERNEL_PAGING_H

#include "libkenv/types.h"

namespace kernel {

constexpr uint64_t PAGE_PRESENT = 1UL << 0;
constexpr uint64_t PAGE_WRITABLE = 1UL << 1;
constexpr uint64_t PAGE_USER = 1UL << 2;
// [신규, 2026-09-18, SP-8D206F11 §2.2] PWT(Page Write-Through, bit3) -
// PAGE_CACHE_DISABLE(PCD, bit4)과 이 비트의 조합이 IA32_PAT의 8개
// 슬롯 중 하나를 고른다(PAT.PCD.PWT 3비트, Paging::initPatForThisCore()
// 문서 주석의 표 참고) - 지금까지는 PCD 단독(인덱스2, UC-)만 썼고
// 이 비트를 세팅하는 코드가 없었다.
constexpr uint64_t PAGE_WRITE_THROUGH = 1UL << 3;
constexpr uint64_t PAGE_CACHE_DISABLE = 1UL << 4;  // MMIO(LAPIC 등)는 반드시 이걸 켜야 한다
// [신규, 2026-09-18, SP-8D206F11 §2.2] PAT(레거시 PAT 비트, 4KiB PTE
// 전용 - 2MiB/1GiB 대형 페이지는 이 대신 bit12를 쓰지만 이 프로젝트의
// kMapPageWithCacheType()은 4KiB 매핑만 다룬다). PAGE_CACHE_DISABLE/
// PAGE_WRITE_THROUGH와 3비트를 조합해 IA32_PAT 인덱스4(WC)를 고를 때
// 세팅한다 - bit7은 대형 페이지 엔트리에서는 PS(Page Size)와 같은
// 자리라 절대 혼동하면 안 된다(SDM 표기 주의, 이 프로젝트에서는
// 4KiB PTE에만 이 상수를 쓴다).
constexpr uint64_t PAGE_PAT = 1UL << 7;

// [신규, 2026-09-16, SP-6BEAE0C1 §2/§11, PN-543C0CE9 착수 6번째 증분]
// Copy-on-Write 표시 - x86_64 페이지 테이블 엔트리의 비트 9-11은
// 하드웨어가 절대 건드리지 않는 "OS 전용" 자리(SDM Vol.3A)라 이 중
// 하나(비트 9)를 골라 썼다. 이 비트가 세팅된 leaf 엔트리는 항상
// `PAGE_WRITABLE`이 꺼져 있어야 하고(그래야 실제 쓰기 시도가 #PF를
// 일으켜 handlePageFault가 가로챌 수 있다), 그 물리 프레임은
// `PageFrameAllocator::retain()`으로 참조 카운트가 매겨져 있어야
// 한다(공유 여부를 이 비트 하나로만 표현하고, 실제 "몇 명이
// 공유하는지"는 참조 카운트가 담당 - 이 비트는 순수하게 "쓰기 폴트가
// 나면 이건 위반이 아니라 복사해야 할 신호"라는 뜻만 가진다). 이
// 프로젝트에 아직 이 비트를 실제로 세팅하는 코드(향후 `fork()`,
// PN-44C91D6E)는 없다 - `Paging::handlePageFault`가 이 비트를 보고
// 반응하는 인프라만 이번 증분에서 미리 갖춘다(§2 COW 설계 스케치가
// 요구한 "posix_spawn은 COW를 안 쓰지만 미래 fork()가 쓸 인프라를
// 지금 준비해 둔다"는 원칙 그대로).
constexpr uint64_t PAGE_COW = 1UL << 9;

// [신규, 2026-09-23, PN-4859FDE9 §7.2 5단계, 설계자 답변 QU-D3A9BF13 -
// "물리 page 엔트리 자체에 AVL 필드가 있을텐데 그거랑 잘 조합해서
// 회수 진행 중/대기 등 임시 상태를 만들어야해"] 회수(스왑아웃) 진행
// 중 임시 상태 - 비트 10(PAGE_COW와 같은 AVL 구간, 아직 아무도 안 씀).
// present=1인 leaf에서만 의미 있고, 항상 PAGE_WRITABLE이 꺼져 있어야
// 한다(그래야 이 프레임에 대한 실제 쓰기 시도가 #PF를 일으켜
// handlePageFault가 가로챌 수 있다 - PAGE_COW와 정확히 같은 관례).
// 읽기는 present 그대로라 폴트 없이 통과한다(디스크 쓰기가 끝날 때까지
// 데이터가 실제로 바뀌지 않으므로 안전) - 오직 "쓰기"만 막아 회수용
// 비동기 디스크 전송 도중 내용이 바뀌는 레이스를 원천 차단한다. 쓰기
// 폴트가 나면(COW 아닌 이 비트) `PageFrameAllocator::cancelReclaimForFrame()`
// 이 그 자리에서 즉시 원상복구(Writable 복원)하므로 새 park/resume
// 메커니즘이 전혀 필요 없다(paging.cpp `kHandleReclaimWriteFault` 참고) -
// 스왑인(읽기 폴트, 아래 §4.2)만 진짜 비동기 대기가 필요하다.
constexpr uint64_t PAGE_RECLAIM_INPROGRESS = 1UL << 10;

// [신규, 2026-09-22, SP-D02C4A73 §4, PN-6D9A5DAE] 스왑 PTE 인코딩 -
// 이 커널 자신의 설계(외부 표준 없음, libswapfs의 온디스크 포맷과는
// 별개 문제). x86_64 PTE는 `Present`(bit0)가 0이면 하드웨어가 나머지
// 63비트를 완전히 무시한다(Intel SDM Vol.3A §4.5, "Not-Present"
// 엔트리는 포맷이 소프트웨어 자유) - `PAGE_COW`(위, bit9)는 항상
// `Present=1`인 leaf 엔트리에만 쓰이므로 이 인코딩과 절대 충돌하지
// 않는다.
//
// Present=0인 PTE에서만 의미를 가진다: 이 비트가 1이면 "스왑아웃된
// 페이지"(슬롯 번호가 비트 12-63에 인코딩돼 있음)라는 뜻이고, 0이면
// 기존 그대로 "진짜 미매핑"(예: 아직 손대지 않은 anonymous VMA 영역,
// 요구 페이징 이전)이다. bit1은 Present=1일 때 `PAGE_WRITABLE`이지만
// Present=0에서는 하드웨어가 안 보므로 재사용에 아무 문제 없다.
constexpr uint64_t PAGE_SWAP_MARKER = 1UL << 1;

// 슬롯 번호는 비트 12~63(52비트) - libswapfs의 `SwapHeaderInfo::lastPage`
// (32비트 상한)와 무관하게 넓게 잡아 둔다(SP-D02C4A73 §3.3 - lastPage는
// "이 스왑 영역 하나의 크기" 제약일 뿐 PTE 인코딩 능력을 좁힐 이유가
// 아님).
constexpr uint64_t kSwapSlotShift = 12;

// [SP-D02C4A73 §4] Present=0으로(PAGE_PRESENT 없이) 슬롯 번호를 인코딩한
// PTE 값을 만든다 - 스왑아웃 경로(rmap 무효화 지점, SP-6CEFBE9B §6.2/
// §7.2)가 present=0으로 바꾸는 그 순간 이 값을 쓴다.
inline uint64_t kMakeSwapPte(uint64_t slot) {
    return (slot << kSwapSlotShift) | PAGE_SWAP_MARKER;
}

// 호출 전 반드시 `(pte & PAGE_SWAP_MARKER) != 0`을 확인해야 한다 -
// 이 함수 자신은 그 확인을 하지 않는다(마커가 꺼진 PTE는 "진짜
// 미매핑"이라 슬롯 번호로 해석하면 안 됨).
inline uint64_t kSwapSlotFromPte(uint64_t pte) {
    return pte >> kSwapSlotShift;
}

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

    // [신규, 2026-09-18, SP-8D206F11 §2.2] IA32_PAT(MSR 0x277)는
    // STAR/LSTAR/SFMASK 등과 마찬가지로 논리 프로세서별(코어별) MSR이라
    // - BSP/AP 모두 각자 이 함수를 불러야 한다(SyscallFastPath::
    // initForThisCore()와 동일한 관례: BSP는 kMain()이 init() 직후,
    // AP는 kApMain()이 SyscallFastPath::initForThisCore()와 같은
    // 자리에서). 인덱스 0~3/5~7은 하드웨어 리셋 기본값 그대로 두고
    // (기존 PAGE_CACHE_DISABLE 사용처가 전혀 영향받지 않음) 인덱스4만
    // WC(Write-Combining)로 재정의한다 - 아래 표(SDM 기본 PAT 인코딩
    // + 이 프로젝트가 바꾸는 부분만 굵게):
    //
    // | 인덱스 | PAT.PCD.PWT | 타입              |
    // |---|---|---|
    // | 0 | 0.0.0 | WB (기존 그대로)          |
    // | 1 | 0.0.1 | WT (기존 그대로)          |
    // | 2 | 0.1.0 | UC- (기존 PAGE_CACHE_DISABLE 단독 사용처가 여기 걸림) |
    // | 3 | 0.1.1 | UC (신규 API의 CacheType::Uncached가 여기)   |
    // | 4 | 1.0.0 | **WC** — [신규]           |
    // | 5~7 | 1.0.1/1.1.0/1.1.1 | 1~3과 동일(기존 그대로) |
    static void initPatForThisCore();

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

    // [SP-6BEAE0C1 §3] 유저 랜드에서 넘어온 포인터/길이를 커널이
    // 역참조하기 전에 검증한다 - "이 프로젝트 전체에 통일된 규약"이
    // 아직 없어(기존 Channel Read/Write 핸들러 전수 확인 결과 지금까지
    // 이 검증 자체가 어디에도 없었다) 새로 추가하는 첫 표준 원시
    // 연산이다. [virtualAddr, virtualAddr+length) 전체가 PML4/PDPT/PD/
    // PT **모든 레벨**에서 PAGE_PRESENT + PAGE_USER를 만족해야 true -
    // x86_64는 어느 한 레벨이라도 U/S 비트가 꺼져 있으면 그 페이지
    // 전체를 supervisor 전용으로 취급하므로(CPU 자체의 권한 판정
    // 방식과 동일), 리프(PT) 엔트리만 보면 안 되고 매 레벨을 실제로
    // 확인해야 한다 - mapPage()가 유저 매핑 시 모든 중간 테이블에도
    // PAGE_USER를 전파해 두므로(kGetOrCreateNextLevel 호출부 참고)
    // 정상적으로 만들어진 유저 페이지는 이 함수를 항상 통과한다.
    // length==0은 검증할 것이 없어 true(빈 범위), virtualAddr+length가
    // 오버플로우하면(악의적 (addr,length) 조합) 즉시 false. pml4Phys
    // 생략 시 현재 CR3(호출자 자신의 주소공간) 기준 - syscall 핸들러가
    // 항상 그 syscall을 제출한 UserThread 자신의 컨텍스트에서 실행되는
    // 이 코드베이스의 관례상 이 기본값이 곧 "그 유저 포인터를 실제로
    // 소유한 프로세스"를 뜻한다.
    static bool isUserRangeValid(uint64_t virtualAddr, uint64_t length, uint64_t pml4Phys = 0);

    // [신규, 2026-09-23, PN-4859FDE9 §4.2] handlePageFault()의 반환값 -
    // 기존 bool(처리함/못함)로는 "스왑인 읽기가 비동기로 진행 중이라
    // 이 스레드를 파킹해야 한다"는 세 번째 결과를 표현할 수 없어 enum
    // 으로 확장했다. idt.cpp가 `ParkForSwapIn`을 받으면 (1) 이미
    // `handlePageFault()` 호출 직후 내린 `gInPageFaultHandler` 재진입
    // 가드를 그대로 둔 채(이미 내려간 뒤이므로 안전), (2)
    // `thread->state = TaskState::Blocked` 후 `Scheduler::parkFromISR
    // (thread, frame)`을 직접 호출한다(반환하지 않음) - `handlePageFault`
    // 자신은 절대 parkFromISR을 부르지 않는다(가드를 내리기 전에
    // IST5를 떠나면 안 되므로, 그 순서를 아는 idt.cpp가 대신 한다).
    enum class PageFaultOutcome : uint8_t {
        NotHandled,    // 진짜 세그폴트 - idt.cpp가 기존처럼 SIGSEGV/패닉 처리
        Handled,       // 그 자리에서 즉시 해결(지연매핑/COW/회수취소) - 재시도하면 성공
        ParkForSwapIn  // 스왑인 읽기 제출 완료, 스레드를 파킹해야 함(위 참고)
    };

    // #PF(vector 14) 핸들러가 호출한다(idt.cpp). faultAddr는 CR2,
    // errorCode는 하드웨어가 스택에 남긴 값 그대로. 처리 결과는 위
    // `PageFaultOutcome` 참고. 이제 네 가지 경우를 처리한다: (1)
    // kLazyZoneBase 범위 안의 not-present 폴트(기존), (2) `PAGE_COW`가
    // 세팅된 present 페이지에 대한 쓰기 위반(기존, PN-543C0CE9) - 새
    // 물리 프레임을 확보해 내용을 복사한 뒤 이 주소공간만 그 새
    // 프레임으로 다시 매핑하고 원래 공유 프레임의 참조 카운트를
    // 줄인다, (3) [신규, PN-4859FDE9 §7.2 5단계] `PAGE_RECLAIM_INPROGRESS`
    // 가 세팅된 present 페이지에 대한 쓰기 위반 - 그 자리에서 즉시
    // 회수를 취소(원상복구)한다(비동기 대기 불필요, 위 비트 문서 주석
    // 참고), (4) [신규, PN-4859FDE9 §4.2] `PAGE_SWAP_MARKER`가 세팅된
    // not-present 페이지에 대한 폴트 - 새 프레임을 확보하고 스왑인
    // 읽기를 비동기로 제출한 뒤 `ParkForSwapIn`을 반환한다(SP-0666DB3C
    // §7이 확정한 "폴트 처리를 또 하나의 대기 항목으로 모델링" 그대로,
    // 다만 syscall의 pendingSyscalls 대신 #DB의 parkFromISR/TCB 재사용
    // 패턴을 그대로 따른다 - PN-EA968DF0/PN-81E49523이 이미 증명한
    // "IST 공유 스택 위에서 안전하게 블로킹하는 유일한 방법"). 그 외
    // 모든 권한 위반은 조용히 덮어쓰지 않고 `NotHandled`로 패스스루한다.
    static PageFaultOutcome handlePageFault(uint64_t faultAddr, uint64_t errorCode);

    // [신규, 2026-09-22, SP-6CEFBE9B §7.2 2/3단계, PN-4859FDE9] 4KiB
    // leaf PTE의 하드웨어 Accessed 비트(bit5)를 읽고, 세팅돼 있었으면
    // 그 자리에서 지운다(second-chance 판정용) - 이미 PAGE_PRESENT인
    // 페이지를 다시 읽고 쓰는 것 자체는 x86_64에서 폴트를 전혀
    // 일으키지 않으므로(CPU가 트랩 없이 조용히 이 비트만 세팅),
    // 재접근 여부를 관찰하는 유일한 방법은 이렇게 주기적으로 직접
    // PTE를 확인하는 것뿐이다(swap 회수 스캔의 전제, §7.2 2단계
    // 문서 주석 참고). 매핑이 없거나(present=0), 2MiB 대형 페이지
    // (PS 비트 - 이 스캔의 anonymous rmap 매핑은 항상 4KiB 단일
    // 페이지뿐이라 범위 밖으로 둠, PageFrameAllocator::retain() 문서
    // 주석과 동일한 전제)면 false. 세팅돼 있던 비트를 지운 뒤에는
    // 이 코어가 지금 이 pml4Phys를 쓰고 있을 때만 로컬 invlpg한다 -
    // 다른 코어의 스테일 TLB는 최악의 경우 다음 스캔 주기에 accessed=1을
    // 한 번 더 관측하게 할 뿐(순수 성능/타이밍 휴리스틱이라 정확성에
    // 영향 없음 - `unmapPage`의 실제 매핑 제거와 달리 이 연산은 데이터
    // 가시성 자체를 바꾸지 않으므로 `TlbShootdown::broadcast()`
    // 같은 크로스 코어 무효화가 필요 없다).
    static bool testAndClearAccessed(uint64_t virtualAddr, uint64_t pml4Phys = 0);

    // [신규, 2026-09-23, PN-4859FDE9 §7.2 5단계] present 4KiB leaf를
    // "회수 진행중"으로 전환한다 - `PAGE_WRITABLE`을 끄고
    // `PAGE_RECLAIM_INPROGRESS`를 세운다(Present는 그대로 유지 - 읽기는
    // 계속 통과, 쓰기만 폴트를 일으켜 `handlePageFault`가 가로챈다).
    // 대상이 present가 아니거나 이미 진행중이면 false(호출부인 회수
    // 스캔은 그 rmap 엔트리를 조용히 건너뛰어야 한다 - 방금 폴트로
    // 사라졌거나 이미 다른 이유로 회수 중일 수 있음, 새 에러 경로를
    // 늘리지 않는다). 이 매핑을 지금 보고 있을 수 있는 모든 코어에
    // `TlbShootdown::broadcast()`로 즉시 전파한다(순수 타이밍 휴리스틱인
    // testAndClearAccessed와 달리 이건 실제 쓰기 보호이므로 스테일 TLB를
    // 남겨두면 안 됨).
    static bool markReclaimInProgress(uint64_t virtualAddr, uint64_t pml4Phys);

    // 회수 취소 - `PAGE_RECLAIM_INPROGRESS`를 끄고 `PAGE_WRITABLE`을
    // 되돌린다(원래 present+writable로 복원). `markReclaimInProgress`가
    // 세워 둔 상태가 아니면 아무 것도 안 한다(방어적 - 이미 완료/취소된
    // 뒤 중복 호출될 수 있음). `PageFrameAllocator::cancelReclaimForFrame()`
    // (쓰기 폴트 즉시 처리)과 회수 완료 핸들러(취소된 채로 끝난 경우)
    // 양쪽이 호출한다.
    static void cancelReclaimInProgress(uint64_t virtualAddr, uint64_t pml4Phys);

    // 회수 완료 - present 리프를 스왑 마커(`kMakeSwapPte(slot)`)로
    // 바꾼다. `markReclaimInProgress`가 세워 둔 상태가 아니면(이미
    // 쓰기 폴트로 취소됐음) 아무 것도 안 한다 - 호출부(회수 완료
    // 핸들러)가 이 반환 여부와 무관하게 항상 먼저
    // `PageFrame::flags & kPageFrameFlagReclaimCanceled`를 확인해야
    // 한다(이 함수 자체는 그 플래그를 모른다, page_frame_allocator.h
    // 참고).
    static void finalizeReclaimToSwap(uint64_t virtualAddr, uint64_t pml4Phys, uint64_t slot);

    // [신규, 2026-09-23, PN-4859FDE9 §4.2] 스왑인 완료 - not-present
    // 스왑 마커 리프를 present+writable+user로 바꾼다. 이미 present면
    // (경쟁하던 다른 스레드/코어의 스왑인이 먼저 끝남 - 같은 프로세스의
    // 다른 스레드나, 이론상 동시에 같은 슬롯을 가리키는 두 폴트가 겹칠
    // 때) false를 반환하고 아무 것도 안 한다 - 호출부는 방금 읽어들인
    // `newPhys`를 대신 `PageFrameAllocator::freePage()`로 반납해야
    // 한다. not-present -> present로 새로 매핑하는 것뿐이라(이전엔
    // 캐싱될 수조차 없었던 자리) 크로스 코어 TLB 무효화가 필요 없다
    // (`mapPage`의 지연 매핑 경로와 동일한 근거).
    static bool completeSwapIn(uint64_t virtualAddr, uint64_t pml4Phys, uint64_t newPhys);

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

// [신규, 2026-09-19, PN-C6CDC26A, 설계자 지시] AsyncTask가 유저
// 포인터를 안전하게 만지는 방법을 "제출자 프로세스의 CR3로 직접
// 전환했다가 되돌리는" 방식(async_task.cpp의
// kSyncCr3ForAsyncExecEntry/kRestoreCr3AfterAsyncExecEntry)에서
// "CR3는 그대로 두고 물리주소 기반으로 직접 접근"하는 방식으로 옮기기
// 위한 공용 원시 연산 - 설계자 지시 원문("제 3자의 입장에서 유저영역을
// 해당 코어에서 구동되는 주소 공간에 맵핑해서 그냥 보면 되는거거든")
// 그대로: `pml4Phys` 기준으로 `userVa`가 실제 유저 매핑돼 있는지
// `Paging::isUserRangeValid()`로 검증하고, `Paging::translatePage()`로
// 얻은 물리 프레임을 `kPhysToVirt()`(모든 CR3에 공유되는 higher-half
// direct map)로 접근한다 - CR3를 단 한 번도 바꾸지 않는다.
//
// **페이지 경계를 넘지 않는다** - `debug_session.cpp`의
// `kCopyDebuggeeMemory()`와 동일한 이유(direct map은 물리적으로
// 연속이지만, 유저 VA가 연속이라고 해서 그 뒤에 있는 물리 프레임까지
// 연속이라는 보장이 전혀 없다). `[userVa, userVa+size)`가 페이지
// 경계를 넘으면 그 경계까지만 유효한 것으로 잘라 돌려준다 - 호출부가
// `outValidLen`(더 긴 범위가 필요하면 그 값만큼만 쓰고 다음 페이지는
// 이 함수를 다시 호출)으로 실제 유효 길이를 받아 나눠 처리할 책임을
// 진다(정확히 kCopyDebuggeeMemory의 chunk 분할 루프와 같은 관례).
//
// 반환값은 그 물리 프레임 안에서 `userVa`가 가리키는 정확한 오프셋의
// **커널 VA**(그대로 역참조 가능) - 매핑이 없거나(`isUserRangeValid`
// 실패) `size==0`이면 `nullptr`, `outValidLen`은 건드리지 않는다.
//
// [갱신, 2026-09-19] 이 헬퍼 자체는 신설했지만, `async_task.cpp`의
// 기존 CR3 스왑 메커니즘(kSyncCr3ForAsyncExecEntry류)은 **아직
// 이 헬퍼로 교체하지 않았다** - 실제 코드 조사 결과, 그 스왑은
// `kAsyncTaskEntryWrapper` 진입 시 "이 onExec() 실행 구간 전체" 동안
// 암묵적으로 CR3가 맞다고 가정하고 유저 포인터를 직접 역참조하는
// 코드베이스 전역 관례(channel.cpp/pnp.cpp/process.cpp/
// debug_session.cpp 등 사실상 모든 AsyncTaskHandler::onExec() 구현)를
// 떠받치고 있어, 그 스왑 자체를 제거하려면 그 모든 소비처를 이
// 헬퍼로 **동시에** 마이그레이션해야 한다 - 이건 계획이 예상했던
// "async_task.cpp 하나만의 좁은 1단계"보다 훨씬 넓은 범위라 설계자
// 확인 없이 임의로 진행하지 않는다(CLAUDE.md 규칙 4) - PN-C6CDC26A
// 본문의 "실제 코드 조사 결과" 절 참고.
void* kResolveUserPointer(uint64_t pml4Phys, uint64_t userVa, uint64_t size, uint64_t* outValidLen);

// [신규, 2026-09-18, SP-8D206F11 §2.3] Paging::initPatForThisCore()가
// 세팅한 IA32_PAT 슬롯 중 소프트웨어가 실제로 고를 수 있는 4개만
// 노출한다(인덱스2 UC-/5~7은 1~3의 미러라 별도 값을 둘 이유가 없음).
enum class CacheType : uint8_t {
    WriteBack,       // 인덱스0 - 일반 RAM 기본값
    WriteThrough,    // 인덱스1
    Uncached,        // 인덱스3 - 기존 PAGE_CACHE_DISABLE 단독 사용처(인덱스2, UC-)와는
                     // 다른 슬롯이지만 실질적 동작은 동일(SP-8D206F11 §2.4 정정 참고)
    WriteCombining,  // 인덱스4 - [신규] 프레임버퍼 등
};

// virtAddr에 physAddr을 매핑하되, flags에 cacheType이 가리키는 PAT
// 인덱스의 PAT/PCD/PWT 비트 조합을 자동으로 얹어 Paging::mapPage()에
// 위임하는 얇은 래퍼(SP-8D206F11 §2.3) - 4KiB 매핑 전용(PAGE_PAT
// 문서 주석 참고, 대형 페이지는 이 API의 대상이 아니다). 새 자료구조/
// 전역 상태 없음 - mapPage()와 마찬가지로 pml4Phys 생략(0) 시 현재
// CR3을 쓴다.
void kMapPageWithCacheType(uint64_t virtAddr, uint64_t physAddr, uint64_t flags, CacheType cacheType,
                            uint64_t pml4Phys = 0);

}  // namespace kernel

#endif  // MINICORE_KERNEL_PAGING_H
