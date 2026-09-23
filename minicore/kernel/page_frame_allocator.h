#ifndef MINICORE_KERNEL_PAGE_FRAME_ALLOCATOR_H
#define MINICORE_KERNEL_PAGE_FRAME_ALLOCATOR_H

#include "hvm_start_info.h"
#include "libkenv/types.h"

namespace kernel {

class Process;  // 포인터로만 참조(RmapEntry::owner) - 전체 정의는 process.h(SP-6BEAE0C1)

constexpr uint32_t kPfaMaxNumaNodes = 8;  // Acpi::kAcpiMaxNumaNodes와 맞춘 상한

// [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §2] PageFrame::flags 비트 -
// 지금 이 증분이 실제로 세팅하는 건 PG_RESERVED뿐이다(bump 예약 구간/
// 커널 이미지 구간). 나머지 넷(PG_HEAD/PG_ACTIVE/PG_ACCESSED/
// PG_SWAPPABLE)은 구조체 계약(§2)의 일부로 지금 값을 확정해 두되,
// 실제로 켜고 끄는 로직(버디 병합, rmap/LRU 삽입·제거)은 후속 증분
// (SP-6CEFBE9B §6.2/§7.2, 이 계획의 3/4번 항목)이 배선한다 - 지금은
// 항상 0으로 남는다.
constexpr uint16_t kPageFrameFlagReserved = 1U << 0;   // PG_RESERVED - 할당 대상 아님
constexpr uint16_t kPageFrameFlagHead = 1U << 1;       // PG_HEAD - order>0 블록의 첫 프레임(페이지 병합용, 미배선)
constexpr uint16_t kPageFrameFlagActive = 1U << 2;     // PG_ACTIVE - active LRU 리스트 소속(미배선)
constexpr uint16_t kPageFrameFlagAccessed = 1U << 3;   // PG_ACCESSED - 최근 참조됨(미배선)
constexpr uint16_t kPageFrameFlagSwappable = 1U << 4;  // PG_SWAPPABLE - rmap/LRU 추적 대상(미배선)
// [신규, 2026-09-23, PN-4859FDE9 §7.2 5단계] 이 프레임의 비동기
// 스왑아웃 쓰기가 지금 진행 중이다 - `SwapReclaimWriteHandler`가
// gLruLock 아래에서 세우고, 그 자신의 완료 시점(성공/취소 무엇이든)에
// 내린다. 이 플래그가 서 있는 동안은 회수 스캔이 이 프레임을 다시
// 회수 후보로 고르면 안 된다(page_frame_allocator.cpp kReclaimScanCallback
// 참고).
constexpr uint16_t kPageFrameFlagReclaiming = 1U << 5;
// [신규, 2026-09-23, PN-4859FDE9 §7.2 5단계] 위 회수가 진행되는 동안
// 다른 스레드가 그 프레임에 실제로 썼다(paging.cpp kHandleReclaimWriteFault가
// 그 자리에서 즉시 세움 - 새 park/resume 불필요, PAGE_RECLAIM_INPROGRESS
// 문서 주석 참고) - `SwapReclaimWriteHandler`가 나중에 쓰기를 완료해도
// 이 플래그를 보면 스왑 전환을 건너뛰고 그냥 회수를 포기한다(이미
// 쓰기 폴트 쪽에서 PTE를 원상복구해 뒀으므로 추가로 할 일 없음).
constexpr uint16_t kPageFrameFlagReclaimCanceled = 1U << 6;

// [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §5] rmap 엔트리 -
// `PageFrame::rmapHead`가 가리키는 단일 연결 리스트의 노드 하나("이
// 프레임이 어떤 프로세스의 어떤 가상주소에 매핑돼 있는지" 한 쌍).
// `GenericSlabAllocator`(SP-D7013B26) 32B 버킷을 그대로 재사용한다
// (새 전용 할당자를 만들지 않음, RM-23F4B687 §4) - 이 증분은 구조체
// 정의만 두고 실제 삽입/제거(SP-6CEFBE9B §6.2)는 후속 증분이 배선한다.
struct RmapEntry {
    Process* owner = nullptr;
    uint64_t virtAddr = 0;
    RmapEntry* next = nullptr;
};

// [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §1] 물리 프레임 하나
// (4KiB)당 정확히 하나 - `PageFrameAllocator::init()`이 direct map
// 크기만큼 이 배열을 커널 이미지 바로 뒤에 bump 예약한다(기존
// `uint16_t` COW 참조 카운트 배열을 대체 - SP-6BEAE0C1 §11-3과 동일한
// 배치 패턴). 캐시 라인 크기(64바이트)에 고정 - 원소 하나의 크기가
// 바뀌면 이미 bump 예약된 배열 전체를 다시 자리 잡아야 하므로(파급력),
// 예비 공간(`reserved`)을 넉넉히 남겨 둔다.
struct PageFrame {
    // --- 참조/공유 ---
    uint16_t refCount = 0;   // 총 참조 카운트(COW+공유 메모리 등) - PageFrameAllocator::retain()/refCount()가 그대로 접근
    uint16_t mapCount = 0;   // 실제 PTE가 가리키는 횟수(SP-6CEFBE9B §6, 아직 미배선)

    // --- 버디/캐시 메타데이터 ---
    uint8_t order = 0;              // 버디 오더, free 블록 head에서만 유효(아직 미배선 - 값은 항상 0)
    uint8_t numaNode = 0;           // Acpi::cpuNumaNode()류와 같은 노드 번호 - init()이 채움
    uint16_t flags = 0;             // 위 kPageFrameFlag* 조합
    uint8_t lastCacheType = 0;      // SP-8D206F11 §2.3 CacheType(아직 미배선)
    uint8_t cacheTypeAssigned = 0;  // 위와 동일

    // --- rmap(SP-6CEFBE9B §6, 아직 미배선 - 항상 nullptr) ---
    RmapEntry* rmapHead = nullptr;

    // --- swap LRU 연결(SP-6CEFBE9B §7, 아직 미배선 - 항상 nullptr) ---
    PageFrame* lruPrev = nullptr;
    PageFrame* lruNext = nullptr;

    // 지금은 어떤 필드도 배정하지 않는다(CLAUDE.md 규칙 4 - 설계 확정
    // 전 임의 배정 금지) - 구조체 stride를 다시 바꾸지 않고 흡수할
    // 여유로 남겨 둔다. 정확한 크기는 위 필드들의 실제 컴파일러 정렬
    // (포인터 필드의 8바이트 경계 패딩 포함)에 따라 정해지므로,
    // 64바이트 전체에서 그 나머지를 그대로 채운다 - 아래
    // static_assert가 어긋나면 이 배열 크기만 조정한다.
    uint8_t reserved[24];
};
static_assert(sizeof(PageFrame) == 64, "PageFrame은 캐시 라인 크기(64바이트)로 고정 - SP-6CEFBE9B §1");

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

    // [SP-39F18E30 §5-B/§5-C/§5-D] physLimit 미만의 물리주소 범위에서만
    // 2^order 페이지 블록을 찾는다(레거시 USB 컨트롤러의 32비트 DMA
    // 제약용 - physLimit=0x1'0000'0000이면 "4GiB 미만"). §5-C가 채택한
    // (a) 선형 탐색 그대로 - 해당 order의 free list만 뒤지고(더 큰
    // 블록을 쪼개지 않음), §5-D에 따라 NUMA 지역성 없이 전체 노드를
    // 순서대로 훑는다(드라이버 초기화 시점에만 일어나는 저빈도 연산이라
    // O(n) 비용을 감수). 실패 시 0.
    static uint64_t allocOrderBelow(uint64_t physLimit, uint32_t order);

    static uint64_t freePageCount();  // 전체 노드 합
    static uint32_t numaNodeCount();
    static uint64_t freePageCountOnNode(uint32_t node);

    // [SP-6BEAE0C1 §2/§11-3] Copy-on-Write 페이지 공유 카운트 - 4KiB
    // 단일 페이지(order 0)에만 의미가 있다(COW로 공유되는 건 항상
    // Vma가 매핑하는 개별 유저 페이지뿐, 커널 스택/테이블 같은 멀티
    // 페이지 블록은 공유 대상이 아니다). retain()을 한 번도 안 부른
    // 페이지는 항상 "추적 안 됨"(소유자 정확히 1개) 상태라 freePage/
    // freeOrder의 기존 동작이 100% 그대로 유지된다 - 이 API를 실제로
    // 쓰는 COW 코드가 생기기 전까지는 아무 호출부에도 영향이 없다.
    //
    // retain()은 이 페이지를 하나 더 공유하기 시작할 때(예: fork()가
    // 부모 페이지를 자식과 읽기전용으로 공유) 부른다 - 처음 부르면
    // 2(기존 소유자 + 새 소유자), 그 다음부터는 +1. freeOrder(order 0)
    // 는 카운트가 0이 아니면 실제로 반납하지 않고 감소만 하다가 0이
    // 되는 순간에만 진짜 free 경로로 넘어간다 - 그래서 카운트가 자연히
    // 0으로 돌아온 뒤에야 free list에 들어가고, 새 소유자가 생기지
    // 않는 한 다시 추적될 일이 없다(allocPage 쪽에서 별도로 리셋할
    // 필요 없음).
    static void retain(uint64_t physAddr);
    static uint32_t refCount(uint64_t physAddr);  // 0 = 추적 안 됨(소유자 1개)

    // [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §1] physAddr가 속한
    // `PageFrame`을 직접 찾는다 - direct map 추적 범위 밖이거나 아직
    // init()이 안 끝났으면 nullptr(방어적, retain()/refCount()가 이미
    // 쓰던 것과 같은 범위 검사).
    static PageFrame* frameFor(uint64_t physAddr);

    // [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §6.2/§7.2] rmap 삽입 -
    // Anonymous 백킹 프레임이 (owner, virtAddr)에 실제로 매핑되는
    // 순간(`ProcessAddressSpaceManager::mapRegion`/`registerFixedRegion`/
    // `resizeAnonymousRegion`이 호출) 부른다. `RmapEntry`를
    // `GenericSlabAllocator`(32B 버킷)로 확보해 `rmapHead`에 push하고
    // `mapCount++`, 이 프레임이 처음 스왑 대상이 되는 순간(`PG_SWAPPABLE`
    // 이 꺼져 있던 상태)이면 `PG_SWAPPABLE`을 켜고 §7.2 1단계대로
    // 전역 inactive 리스트 뒤에 넣는다. physAddr가 추적 범위 밖이거나
    // 슬랩 고갈이면 false(호출부는 매핑 자체를 되돌릴 필요는 없다 -
    // 이 실패는 "회수 후보 목록에서 빠짐"일 뿐 매핑 자체의 유효성과
    // 무관하다, RM-23F4B687 §4 - 새 에러 경로를 늘리지 않는다).
    static bool insertRmap(uint64_t physAddr, Process* owner, uint64_t virtAddr);

    // rmap 제거 - `(owner, virtAddr)`와 정확히 일치하는 엔트리 하나만
    // 찾아 제거하고 `mapCount--`(리스트가 프레임당이라 못 찾아도
    // 조용히 아무 일도 안 함 - 방어적). `unmapRegion`/`unmapAll`/
    // `resizeAnonymousRegion`(축소)이 실제 페이지 해제(`freePage`)
    // **이전에** 불러야 한다 - `freeOrder`가 프레임을 진짜로 버디
    // 목록에 되돌리는 순간 남은 rmap 엔트리를 전부 강제로 청소하지만
    // (방어적 마지막 안전망), 정상 경로는 이 함수로 먼저 깨끗하게
    // 정리하는 쪽이다.
    static void removeRmap(uint64_t physAddr, Process* owner, uint64_t virtAddr);

    // [신규, 2026-09-22, PN-4859FDE9, SP-6CEFBE9B §8-1] swap 회수
    // 스캔의 부팅 시 진입점 - 전용 Task/KernelThread를 만들지 않고
    // (QU-A8C0CC2C 설계자 답변) `DelayedExecutionQueue`(SP-F15B4A63
    // §3)에 1회성 콜백으로 등록한다. 그 콜백은 실행이 끝날 때마다
    // 스스로를 다시 등록(self-rearm)해 주기적 동작을 흉내낸다 - 부팅
    // 중 한 번만 호출(BSP, `PageFrameAllocator::init()`과
    // `DelayedExecutionQueue::init()` 둘 다 끝난 뒤 아무 때나).
    //
    // **[범위, 2026-09-22, QU-41F78A3E 데드락 위험 발견]** 이번 증분은
    // §7.2 2/3/4단계(PTE Accessed 비트 스캔으로 재접근 감지 →
    // inactive→active 승격, active 리스트 비대화 시 강등)까지만
    // 구현한다 - **5단계(실제 회수+스왑 쓰기)는 포함하지 않는다.**
    // `DelayedExecutionQueue` 콜백이 `AsyncReactor::drainOnce()` 안에서
    // 중첩 실행되는데(그 시점 재진입 방지 가드가 이미 세팅됨),
    // `SwapBackend::writeSlot()`의 블로킹 대기가 바로 그 가드에 막혀
    // 데드락하는 실측 확인된 위험 때문 - 자세한 근거는 `QU-41F78A3E`
    // (SP-6CEFBE9B 대상)/`PN-4859FDE9` 참고, 설계자 답변 대기 중이다.
    static void startReclaimScan();

    // [신규, 2026-09-23, PN-4859FDE9 §7.2 5단계] `frame`의 비동기
    // 스왑아웃 쓰기가 진행 중인 동안 그 프레임에 대한 쓰기 폴트가
    // 발생했을 때 즉시 호출한다(paging.cpp `kHandleReclaimWriteFault`) -
    // gLruLock 아래에서 이 프레임의 모든 rmap 엔트리를 `Paging::
    // cancelReclaimInProgress()`로 원상복구(Writable 복원)하고
    // `kPageFrameFlagReclaimCanceled`를 세운다. 나중에 끝나는
    // `SwapReclaimWriteHandler`가 그 플래그를 보고 스왑 전환(PTE를
    // 스왑 마커로) 대신 그냥 포기하게 만드는 게 이 함수의 유일한
    // 목적 - 이미 이 함수가 PTE를 되돌려 놨으므로 그 외에 할 일은
    // 없다. `kPageFrameFlagReclaiming`이 꺼져 있거나 이미
    // `kPageFrameFlagReclaimCanceled`가 서 있으면(레이스로 두 번
    // 불릴 수 있음 - 예: 같은 프레임을 공유하는 두 rmap 엔트리가
    // 동시에 쓰기 폴트) 아무 것도 안 한다.
    static void cancelReclaimForFrame(PageFrame* frame);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PAGE_FRAME_ALLOCATOR_H
