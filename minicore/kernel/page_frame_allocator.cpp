#include "page_frame_allocator.h"

#include "acpi.h"
#include "async_task.h"
#include "block_device.h"
#include "delayed_exec.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"
#include "libswapfs/swapfs.h"
#include "swap_backend.h"
#include "paging.h"
#include "process.h"

namespace {

constexpr kernel::uint64_t kPageSize = 4096;
constexpr kernel::uint32_t kMaxOrder = 10;  // 4KiB << 10 = 4MiB 최대 블록
constexpr kernel::uint64_t kLowReservedEnd = 0x200000;      // 2MiB: BIOS 영역 + 커널 자신

// next는 다음 블록의 "물리주소"다(가상 포인터 아님) - 0이면 끝.
// 널 페이지(물리주소 0)는 kLowReservedEnd 블랭킷 예약에 항상 포함돼
// 실제 블록으로 절대 안 쓰이므로 sentinel로 안전하다. 물리주소를
// 그대로 저장/비교해야 Paging::directMapLimit()을 넘어서도
// (PL-99562483/PN-4AA5425D) 값 자체는 그대로 유효하다 - 실제로 읽고
// 쓸 때만 kPhysToVirt를 거친다.
struct FreeBlock {
    kernel::uint64_t next;
};

struct PfaNode {
    kernel::uint64_t freeListHeads[kMaxOrder + 1];  // 물리주소, 0 = 비어있음
    kernel::uint64_t freePageCount;
    // 이 노드의 free list를 건드리는 공개 API(allocOrderOnNode/
    // freeOrder) 진입점 하나당 한 번만 잠근다 - kObtainBlock의 내부
    // 재귀는 이미 잠긴 상태로 도는 거라 다시 잠그지 않는다(SMP 0단계,
    // QU-B97FDA44, 2026-09-14).
    kernel::Spinlock lock;
};

PfaNode gNodes[kernel::kPfaMaxNumaNodes];
kernel::uint32_t gNodeCount = 1;

// [수정, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §1] 기존 "COW 참조
// 카운트 하나만 있는 uint16_t 배열"(SP-6BEAE0C1 §11-3)을 여러 필드를
// 가진 `PageFrame` 구조체 배열로 대체 - 프레임 개수는 실행 중 실측한
// Paging::directMapLimit()(4~512GiB 동적)에 맞춰 init()에서 정해지므로
// 정적 배열이 아니라 커널 이미지 바로 뒤 물리 공간을 bump 방식으로
// 예약해 둔다(직접 매핑 범위 안이라 별도 매핑 없이 kPhysToVirt로 바로
// 접근 가능 - Paging::init()이 이 호출보다 먼저 전체 direct map을
// 이미 확정해 둠). 배치 패턴 자체는 기존과 동일 - 원소 크기만
// 2바이트에서 64바이트로 늘었다.
kernel::PageFrame* gPageFrames = nullptr;
kernel::uint64_t gPageFrameCount = 0;

// [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §7.1] swap LRU 전역
// active/inactive 이중 연결 리스트 - `PageFrame::lruPrev`/`lruNext`로
// 스레딩한다(새 자료구조 추가 없음, Linux `struct page`의 `lru`
// 필드와 같은 패턴). 이 증분은 §7.2 1단계(신규 매핑 시 inactive 뒤에
// 삽입)만 배선한다 - 승격/강등/회수(2-5단계)는 스캔 트리거 자체가
// 아직 없어 범위 밖(SP-6CEFBE9B §7.3, §8 항목1).
struct PageFrameList {
    kernel::PageFrame* head = nullptr;
    kernel::PageFrame* tail = nullptr;
    // [신규, 2026-09-22, PN-4859FDE9] §7.2 4단계(강등) 판정이
    // "active가 inactive보다 큰가"를 매 스캔마다 물어야 하는데,
    // 리스트를 매번 끝까지 세면 배치 크기와 무관하게 O(전체 길이)
    // 비용이 든다 - kLruPushBack/kLruUnlink가 자동으로 유지하는 카운터.
    kernel::uint32_t count = 0;
};
PageFrameList gActiveList;
PageFrameList gInactiveList;

// [신규, 2026-09-22, PN-4859FDE9] gActiveList/gInactiveList와 그
// 소속을 뜻하는 PageFrame::flags(PG_ACTIVE/PG_SWAPPABLE/PG_ACCESSED)/
// rmap 연결 리스트(PageFrame::rmapHead)를 함께 보호한다 - 지금까지
// (insertRmap/removeRmap/freeOrder) 이 구조들을 건드리는 곳 어디에도
// 락이 없었다(단일 코어 가정 잔재로 보임) - 이번 증분이 새 동시
// 접근자(회수 스캔, 매 코어의 idle 루프에서 주기적으로 실행)를
// 추가하면서 실제로 위험해지는 기존 SMP 공백이라 함께 고친다.
// **락 순서**: 이 락을 쥔 채로 `Paging::testAndClearAccessed()`(내부적으로
// 코어별 주소공간 락을 따로 잡음)를 부르는 지점이 있다(아래 스캔
// 본문) - 반대 방향(주소공간 락을 쥔 채 이 락을 시도)으로 들어오는
// 호출은 없음을 확인했다(`insertRmap`/`removeRmap`은 `Paging::
// mapPage`/`unmapPage`가 이미 반환해 그 내부 락을 놓은 뒤에만
// 호출됨, `address_space.cpp` 호출부 확인) - 그래서 항상 "이 락 →
// 주소공간 락" 한 방향으로만 중첩되고 교착 위험이 없다.
kernel::Spinlock gLruLock;

void kLruPushBack(PageFrameList& list, kernel::PageFrame* frame) {
    frame->lruPrev = list.tail;
    frame->lruNext = nullptr;
    if (list.tail) {
        list.tail->lruNext = frame;
    } else {
        list.head = frame;
    }
    list.tail = frame;
    ++list.count;
}

void kLruUnlink(PageFrameList& list, kernel::PageFrame* frame) {
    if (frame->lruPrev) {
        frame->lruPrev->lruNext = frame->lruNext;
    } else {
        list.head = frame->lruNext;
    }
    if (frame->lruNext) {
        frame->lruNext->lruPrev = frame->lruPrev;
    } else {
        list.tail = frame->lruPrev;
    }
    frame->lruPrev = nullptr;
    frame->lruNext = nullptr;
    --list.count;
}

kernel::uint64_t kAlignUp(kernel::uint64_t value, kernel::uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

kernel::uint64_t kAlignDown(kernel::uint64_t value, kernel::uint64_t align) {
    return value & ~(align - 1);
}

FreeBlock* kAsBlock(kernel::uint64_t physAddr) {
    return reinterpret_cast<FreeBlock*>(kernel::kPhysToVirt(physAddr));
}

void kInsertBlock(PfaNode& node, kernel::uint64_t addr, kernel::uint32_t order) {
    kAsBlock(addr)->next = node.freeListHeads[order];
    node.freeListHeads[order] = addr;
}

bool kTryRemoveBlock(PfaNode& node, kernel::uint64_t addr, kernel::uint32_t order) {
    kernel::uint64_t* cur = &node.freeListHeads[order];
    while (*cur) {
        if (*cur == addr) {
            *cur = kAsBlock(*cur)->next;
            return true;
        }
        cur = &kAsBlock(*cur)->next;
    }
    return false;
}

kernel::uint64_t kPopBlock(PfaNode& node, kernel::uint32_t order) {
    const kernel::uint64_t addr = node.freeListHeads[order];
    if (!addr) {
        return 0;
    }
    node.freeListHeads[order] = kAsBlock(addr)->next;
    return addr;
}

kernel::uint64_t kBuddyAddr(kernel::uint64_t addr, kernel::uint32_t order) {
    return addr ^ (kPageSize << order);
}

// 지정한 order의 블록을 하나 확보한다 - 없으면 한 단계 큰 블록을
// 재귀적으로 얻어 반으로 쪼개고(짝 하나는 그 order 리스트에 도로
// 넣음), 전체 free 카운트는 여기서 건드리지 않는다(쪼개도 총량은
// 그대로라서 - 카운트 조정은 공개 API에서 한 번만 한다).
kernel::uint64_t kObtainBlock(PfaNode& node, kernel::uint32_t order) {
    if (order > kMaxOrder) {
        return 0;
    }
    kernel::uint64_t addr = kPopBlock(node, order);
    if (addr) {
        return addr;
    }
    kernel::uint64_t bigger = kObtainBlock(node, order + 1);
    if (!bigger) {
        return 0;
    }
    kernel::uint64_t buddy = bigger + (kPageSize << order);
    kInsertBlock(node, buddy, order);
    return bigger;
}

void kAddRegionToBuddy(PfaNode& node, kernel::uint64_t start, kernel::uint64_t end) {
    start = kAlignUp(start, kPageSize);
    end = kAlignDown(end, kPageSize);
    while (start < end) {
        kernel::uint32_t order = kMaxOrder;
        while (order > 0) {
            const kernel::uint64_t blockSize = kPageSize << order;
            if ((start % blockSize) == 0 && (start + blockSize) <= end) {
                break;
            }
            --order;
        }
        const kernel::uint64_t blockSize = kPageSize << order;
        kInsertBlock(node, start, order);
        node.freePageCount += (1UL << order);
        start += blockSize;
    }
}

struct Range {
    kernel::uint64_t start;
    kernel::uint64_t end;
};

constexpr int kMaxRanges = 64;

// ranges[0..count)에서 [resStart, resEnd)와 겹치는 부분을 전부
// 잘라낸다 - 겹치는 range는 앞쪽 조각으로 축소(또는 완전히 없어짐)
// 되고, 뒤쪽 조각이 남으면 목록 끝에 새로 추가한다.
void kSubtractReservedFromList(Range* ranges, int& count, kernel::uint64_t resStart, kernel::uint64_t resEnd) {
    if (resStart >= resEnd) {
        return;
    }
    const int originalCount = count;
    for (int i = 0; i < originalCount; ++i) {
        const Range r = ranges[i];
        if (resEnd <= r.start || resStart >= r.end) {
            continue;  // 안 겹침
        }
        if (resStart > r.start) {
            ranges[i] = {r.start, resStart};
        } else {
            ranges[i] = {0, 0};  // 앞쪽 조각 없음
        }
        if (resEnd < r.end && count < kMaxRanges) {
            ranges[count++] = {resEnd, r.end};
        }
    }
}

// 어떤 물리주소가 어느 노드에 속하는지 나중에(freePage 시점에) 다시
// 찾을 수 있도록 배정 결과를 기록해 둔다.
struct RangeNode {
    kernel::uint64_t start;
    kernel::uint64_t end;
    kernel::uint32_t node;
};
constexpr int kMaxRangeNodes = 128;
RangeNode gRangeNodeMap[kMaxRangeNodes];
int gRangeNodeMapCount = 0;

// [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §1] [start,end) 범위의
// 각 PageFrame::numaNode를 채운다 - kAssignRangeToNode가 실제로 그
// 범위를 어느 노드의 buddy 트리에 넣기로 확정한 직후에만 부른다
// (kPartitionRangeByAffinity가 이미 SRAT 기준으로 쪼개 둔 조각이라
// 여기서 다시 노드를 판정할 필요는 없다 - 그대로 받아쓴다).
void kSetPageFrameNode(kernel::uint64_t start, kernel::uint64_t end, kernel::uint32_t node) {
    if (!gPageFrames) {
        return;
    }
    const kernel::uint64_t startFrame = start / kPageSize;
    const kernel::uint64_t endFrame = (end + kPageSize - 1) / kPageSize;
    for (kernel::uint64_t f = startFrame; f < endFrame && f < gPageFrameCount; ++f) {
        gPageFrames[f].numaNode = static_cast<kernel::uint8_t>(node);
    }
}

// [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §2] [start,end) 범위의
// 각 PageFrame::flags에 flag를 켠다(끄기는 아직 이 증분의 소비자가
// 없어 별도 함수를 안 둔다 - RM-23F4B687 §4). start/end는 정렬 안 돼
// 있어도 안전(내부에서 4KiB 단위로 올림/내림).
void kSetPageFrameFlags(kernel::uint64_t start, kernel::uint64_t end, kernel::uint16_t flag) {
    if (!gPageFrames || start >= end) {
        return;
    }
    const kernel::uint64_t startFrame = start / kPageSize;
    const kernel::uint64_t endFrame = (end + kPageSize - 1) / kPageSize;
    for (kernel::uint64_t f = startFrame; f < endFrame && f < gPageFrameCount; ++f) {
        gPageFrames[f].flags |= flag;
    }
}

void kAssignRangeToNode(kernel::uint64_t start, kernel::uint64_t end, kernel::uint32_t node) {
    if (start >= end) {
        return;
    }
    if (node >= gNodeCount) {
        node = 0;  // 방어적 fallback
    }
    kAddRegionToBuddy(gNodes[node], start, end);
    kSetPageFrameNode(start, end, node);
    if (gRangeNodeMapCount < kMaxRangeNodes) {
        gRangeNodeMap[gRangeNodeMapCount++] = {start, end, node};
    }
}

// 지금 실행 중인 코어의 APIC ID로 Acpi가 SRAT에서 알아낸 소속 노드를
// 찾는다 - 못 찾으면(코어가 MADT에 없거나 SRAT 정보가 아예 없거나)
// 노드0으로 방어적으로 떨어진다. Lapic::init()이 아직 안 끝났으면
// (자기 자신을 매핑하려고 이 할당자를 부르는 경우 포함 - 닭-달걀
// 문제, 2026-09-14 실측으로 발견) id()를 부르지 않고 그냥 노드0을
// 쓴다 - BSP는 관례상 거의 항상 노드0이라 안전한 기본값이다.
kernel::uint32_t kCurrentNumaNode() {
    if (!kernel::Lapic::isReady()) {
        return 0;
    }
    const kernel::uint32_t myApicId = kernel::Lapic::id();
    const kernel::uint32_t cpuCount = kernel::Acpi::cpuCount();
    for (kernel::uint32_t i = 0; i < cpuCount; ++i) {
        if (kernel::Acpi::cpuApicId(i) == myApicId) {
            return kernel::Acpi::cpuNumaNode(i);
        }
    }
    return 0;
}

kernel::uint32_t kNodeForAddress(kernel::uint64_t addr) {
    for (int i = 0; i < gRangeNodeMapCount; ++i) {
        if (addr >= gRangeNodeMap[i].start && addr < gRangeNodeMap[i].end) {
            return gRangeNodeMap[i].node;
        }
    }
    return 0;  // 못 찾으면 방어적으로 노드0(있을 수 없는 경우 - 우리가 준 주소만 free될 것이므로)
}

// SRAT 메모리 어피니티 테이블로 range를 노드별 조각으로 나눈다 -
// 어떤 어피니티 엔트리에도 안 걸리는 부분은 노드0으로 떨어진다
// (정보 없음 fallback, kSubtractReservedFromList와 같은 조각내기
// 패턴을 "빼기"가 아니라 "겹치는 부분 추출"로 재사용한다).
void kPartitionRangeByAffinity(kernel::uint64_t rangeStart, kernel::uint64_t rangeEnd) {
    Range remaining[kMaxRanges];
    int remainingCount = 1;
    remaining[0] = {rangeStart, rangeEnd};

    const kernel::uint32_t affinityCount = kernel::Acpi::memoryAffinityCount();
    for (kernel::uint32_t a = 0; a < affinityCount && remainingCount > 0; ++a) {
        const kernel::uint64_t affBase = kernel::Acpi::memoryAffinityBase(a);
        const kernel::uint64_t affEnd = affBase + kernel::Acpi::memoryAffinityLength(a);
        const kernel::uint32_t affNode = kernel::Acpi::memoryAffinityNode(a);

        Range next[kMaxRanges];
        int nextCount = 0;
        for (int i = 0; i < remainingCount; ++i) {
            const kernel::uint64_t pStart = remaining[i].start;
            const kernel::uint64_t pEnd = remaining[i].end;
            const kernel::uint64_t ovStart = pStart > affBase ? pStart : affBase;
            const kernel::uint64_t ovEnd = pEnd < affEnd ? pEnd : affEnd;
            if (ovStart < ovEnd) {
                kAssignRangeToNode(ovStart, ovEnd, affNode);
                if (pStart < ovStart && nextCount < kMaxRanges) {
                    next[nextCount++] = {pStart, ovStart};
                }
                if (ovEnd < pEnd && nextCount < kMaxRanges) {
                    next[nextCount++] = {ovEnd, pEnd};
                }
            } else if (nextCount < kMaxRanges) {
                next[nextCount++] = remaining[i];
            }
        }
        for (int i = 0; i < nextCount; ++i) {
            remaining[i] = next[i];
        }
        remainingCount = nextCount;
    }

    for (int i = 0; i < remainingCount; ++i) {
        kAssignRangeToNode(remaining[i].start, remaining[i].end, 0);
    }
}

// ---------------------------------------------------------------------
// [신규, 2026-09-22, PN-4859FDE9, SP-6CEFBE9B §7.2 2/3/4단계 + §8-1]
// swap 회수 스캔 - 5단계(실제 회수+스왑 쓰기)는 QU-41F78A3E 데드락
// 위험 발견으로 보류(gLruLock 문서 주석 참고) - 이 스캔은 재접근
// 감지(Accessed 비트 PTE-walk)로 inactive→active 승격과, active
// 리스트가 비대해지면 강등(aging)만 수행한다. 전부 순수 페이지
// 테이블 조작(Paging::testAndClearAccessed)뿐이라 블로킹 I/O가
// 전혀 없다 - drainOnce() 중첩 컨텍스트에서 안전하게 실행 가능.
// ---------------------------------------------------------------------

// 실측 후 조정 대상(SP-6CEFBE9B §8-1.5, RM-23F4B687 §4) - v1 기본값.
constexpr kernel::uint64_t kReclaimScanIntervalTicks = 100;  // Timer 100Hz 기준 약 1초
constexpr kernel::uint32_t kReclaimScanBatchSize = 16;       // 스캔/강등 각각의 1회 상한

// inactive 리스트 head(가장 오래 전에 들어온 쪽 - kLruPushBack이
// 항상 tail에 넣으므로 head가 가장 오래된 항목이다)부터 최대
// kReclaimScanBatchSize개를 훑어, 프레임의 rmap 엔트리 전부에 대해
// Accessed 비트를 확인한다(§7.2 2단계) - 하나라도 세팅돼 있었으면
// "재접근됨"으로 판정해 inactive에서 빼 active로 승격한다(3단계,
// second-chance - PG_ACTIVE 세팅, PG_ACCESSED는 지운 상태로 시작).
// 세팅된 비트는 발견하는 즉시(승격 여부와 무관하게) 전부 지운다 -
// 다음 스캔에서 정확히 재판정되려면 이번 관찰을 소모해야 한다.
void kReclaimScanPromotePass() {
    kernel::PageFrame* cursor = gInactiveList.head;
    kernel::uint32_t scanned = 0;
    while (cursor && scanned < kReclaimScanBatchSize) {
        kernel::PageFrame* next = cursor->lruNext;  // 승격 시 cursor 자신의 링크가 끊기므로 미리 저장
        bool referenced = false;
        for (kernel::RmapEntry* rmap = cursor->rmapHead; rmap; rmap = rmap->next) {
            if (kernel::Paging::testAndClearAccessed(rmap->virtAddr, rmap->owner->pml4Phys)) {
                referenced = true;  // 계속 순회 - 나머지 rmap 엔트리의 비트도 전부 지워야 함(위 문서 주석)
            }
        }
        if (referenced) {
            kLruUnlink(gInactiveList, cursor);
            cursor->flags = static_cast<kernel::uint16_t>((cursor->flags | kernel::kPageFrameFlagActive) &
                                                            ~kernel::kPageFrameFlagAccessed);
            kLruPushBack(gActiveList, cursor);
        }
        cursor = next;
        ++scanned;
    }
}

// active 리스트가 inactive보다 커지면(§7.2 4단계 - 정확한 목표 비율은
// §8-1.5가 "실측 후 조정"으로 열어 둠, v1은 "1:1 이하 유지"를 채택)
// head(가장 오래된 쪽)부터 최대 kReclaimScanBatchSize개를 inactive로
// 되돌린다(aging) - 되돌아간 프레임은 다시 "의심 대상"이 되어 다음
// 스캔의 승격 패스에서 재평가된다(§7.2 1단계의 "새 진입" 취급과
// 동일한 대우).
void kReclaimScanDemotePass() {
    kernel::uint32_t demoted = 0;
    while (demoted < kReclaimScanBatchSize && gActiveList.count > gInactiveList.count) {
        kernel::PageFrame* oldest = gActiveList.head;
        if (!oldest) {
            break;
        }
        kLruUnlink(gActiveList, oldest);
        oldest->flags = static_cast<kernel::uint16_t>(oldest->flags & ~kernel::kPageFrameFlagActive);
        kLruPushBack(gInactiveList, oldest);
        ++demoted;
    }
}

// [신규, 2026-09-23, PN-4859FDE9 §7.2 5단계] 실제 회수(스왑아웃) 쓰기
// 완료 처리 - 쓰기 완료 시점에 이 프레임의 회수를 확정(스왑 마커
// 전환+물리 프레임 반납)할지 포기할지 결정하고 후속 조치를 전부
// 끝낸다(kEnsureSwapReclaimWriteHandlerRegistered 아래 참고 - 완료를
// 관측할 별도 "제출자"가 없는 fire-and-forget 제출이라, swapfs_io.h의
// SwapWriteHandler와 달리 이 핸들러 하나가 후속 조치까지 전부 진다).
struct SwapReclaimArgs {
    fs::BlockDevice* device = nullptr;
    fs::SwapSlot slot = 0;
    kernel::PageFrame* frame = nullptr;
    kernel::uint64_t physAddr = 0;
};

class SwapReclaimWriteHandler : public kernel::AsyncTaskHandler {
public:
    kernel::AsyncExecCoro onExec(kernel::AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<SwapReclaimArgs*>(argsRaw);

        const kernel::uint32_t blockSize = args->device->blockSize();
        const kernel::uint32_t blocksPerPage = static_cast<kernel::uint32_t>(fs::kSwapPageSize / blockSize);
        const kernel::uint64_t lba = (args->slot * fs::kSwapPageSize) / blockSize;
        void* srcPage = reinterpret_cast<void*>(kernel::kPhysToVirt(args->physAddr));

        fs::BlockIoResult ioResult;
        kernel::AsyncTask* ioTask = args->device->submitWriteBlocks(lba, srcPage, blocksPerPage, &ioResult);
        if (ioTask) {
            co_await kernel::AsyncTaskCoroAwaiter(ioTask);
        } else {
            ioResult.ok = false;
        }

        // 성공했고 그 사이 취소되지도 않았을 때만 실제로 물리 프레임을
        // 반납한다 - 그 외(디스크 쓰기 실패, 또는 쓰기 폴트로 이미
        // 취소됨)엔 슬롯만 돌려주고 프레임은 그대로 살려 둔다(실패를
        // 영구 에러로 승격하지 않는다 - RM-23F4B687 §4, 다음 스캔에서
        // 다시 회수 후보가 될 뿐).
        bool reclaimedSuccessfully = false;
        {
            kernel::SpinlockGuard guard(gLruLock);
            kernel::PageFrame* frame = args->frame;
            const bool canceled = (frame->flags & kernel::kPageFrameFlagReclaimCanceled) != 0;
            frame->flags = static_cast<kernel::uint16_t>(
                frame->flags & ~(kernel::kPageFrameFlagReclaiming | kernel::kPageFrameFlagReclaimCanceled));

            if (ioResult.ok && !canceled) {
                reclaimedSuccessfully = true;
                for (kernel::RmapEntry* rmap = frame->rmapHead; rmap; rmap = rmap->next) {
                    kernel::Paging::finalizeReclaimToSwap(rmap->virtAddr, rmap->owner->pml4Phys, args->slot);
                }
                // 더 이상 이 물리 프레임을 매핑하는 곳이 없다(전부
                // 스왑 마커로 바뀜) - kHandleCowWriteFault의 "removeRmap
                // 먼저, freePage 나중" 순서와 동일하게, rmap 리스트를
                // 먼저 비운 뒤(아래 루프 이후) freePage를 이 임계구역
                // 밖에서 부른다.
                kernel::RmapEntry* entry = frame->rmapHead;
                while (entry) {
                    kernel::RmapEntry* next = entry->next;
                    kernel::GenericSlabAllocator::free(entry, sizeof(kernel::RmapEntry));
                    entry = next;
                }
                frame->rmapHead = nullptr;
                frame->mapCount = 0;
                frame->flags = static_cast<kernel::uint16_t>(
                    frame->flags &
                    ~(kernel::kPageFrameFlagActive | kernel::kPageFrameFlagAccessed | kernel::kPageFrameFlagSwappable));
                // frame은 이미 kReclaimScanReclaimPass()가 kLruUnlink해
                // 뒀으므로 여기서 리스트 조작이 필요 없다.
            } else {
                // 실패 또는 취소 - PTE는 이미 원상복구돼 있다(실패:
                // 아래에서, 취소: kHandleReclaimWriteFault가 이미) -
                // 다시 회수 후보 목록(inactive tail)으로 되돌린다.
                if (!ioResult.ok) {
                    for (kernel::RmapEntry* rmap = frame->rmapHead; rmap; rmap = rmap->next) {
                        kernel::Paging::cancelReclaimInProgress(rmap->virtAddr, rmap->owner->pml4Phys);
                    }
                }
                kLruPushBack(gInactiveList, frame);
            }
        }

        fs::SwapBackend* backend = kernel::kActiveSwapBackend();
        if (reclaimedSuccessfully) {
            kernel::PageFrameAllocator::freePage(args->physAddr);
        } else if (backend) {
            backend->freeSlot(args->slot);
        }

        kernel::GenericSlabAllocator::free(args, sizeof(SwapReclaimArgs));
        co_return;
    }
    void onFailure(kernel::AsyncTask*) override {}
    void onCancel(kernel::AsyncTask*, void*) override {}
};

SwapReclaimWriteHandler gSwapReclaimWriteHandler;
kernel::AsyncTaskSubjectCode gSwapReclaimWriteSubjectCode = 0;
bool gSwapReclaimWriteHandlerRegistered = false;

kernel::AsyncTaskSubjectCode kEnsureSwapReclaimWriteHandlerRegistered() {
    if (!gSwapReclaimWriteHandlerRegistered) {
        gSwapReclaimWriteSubjectCode = kernel::AsyncCallbackRegistry::registerHandler(&gSwapReclaimWriteHandler);
        gSwapReclaimWriteHandlerRegistered = true;
    }
    return gSwapReclaimWriteSubjectCode;
}

// [신규, 2026-09-23, PN-4859FDE9 §7.2 5단계] inactive head부터 최대
// kReclaimScanBatchSize개를 훑어 실제 회수를 시작한다 - 이미 회수
// 진행중이거나(kPageFrameFlagReclaiming) 공유 중인(refCount!=0, COW 등
// - 여러 프로세스의 매핑이 걸려 있어 v1 회수 대상이 아님, PN-4859FDE9
// 본문 "정직한 v1 한계" 참고) 프레임은 건너뛴다. 대상마다 (1) 슬롯
// 할당(순수 비트맵 연산, I/O 없음 - gLruLock 아래에서 안전), (2) rmap
// 전체를 `Paging::markReclaimInProgress()`로 전환, (3) inactive에서
// 빼고(`kPageFrameFlagReclaiming` 세움) `SwapReclaimWriteHandler`에
// fire-and-forget 제출. 슬롯 고갈이면 이번 스캔은 그 자리에서 끝낸다.
//
// [[maybe_unused]]: 위 kReclaimScanCallback()이 실제로 이 함수를
// 부르는 한 줄을 parkFromISR 행 버그 때문에 주석 처리해 뒀다(그
// 문서 주석 참고) - 이 함수 자체는 스왑아웃 쓰기 경로로서는 실측
// 검증이 끝난 상태라 그대로 남겨 둔다, -Wunused-function만 잠재운다.
[[maybe_unused]] void kReclaimScanReclaimPass() {
    fs::SwapBackend* backend = kernel::kActiveSwapBackend();
    if (!backend) {
        return;  // 스왑 백엔드가 없으면 회수 자체가 무의미 - v1은 스왑 없이도 그냥 계속 동작(조용히 건너뜀)
    }
    kernel::PageFrame* cursor = gInactiveList.head;
    kernel::uint32_t scanned = 0;
    while (cursor && scanned < kReclaimScanBatchSize) {
        kernel::PageFrame* next = cursor->lruNext;
        ++scanned;
        if (cursor->flags & kernel::kPageFrameFlagReclaiming) {
            cursor = next;
            continue;
        }
        const kernel::uint64_t physAddr =
            static_cast<kernel::uint64_t>(cursor - gPageFrames) * kPageSize;
        if (kernel::PageFrameAllocator::refCount(physAddr) != 0) {
            cursor = next;  // 공유 프레임 - v1 회수 대상 아님
            continue;
        }

        fs::SwapSlot slot = 0;
        if (!backend->allocateSlot(&slot)) {
            break;  // 스왑 공간 고갈 - 이번 스캔은 여기서 끝
        }

        bool anyMarked = false;
        for (kernel::RmapEntry* rmap = cursor->rmapHead; rmap; rmap = rmap->next) {
            if (kernel::Paging::markReclaimInProgress(rmap->virtAddr, rmap->owner->pml4Phys)) {
                anyMarked = true;
            }
        }
        if (!anyMarked) {
            // rmap이 비어 있거나 전부 실패(예: 방금 폴트로 사라짐) -
            // 슬롯을 반납하고 다음 후보로.
            backend->freeSlot(slot);
            cursor = next;
            continue;
        }

        auto* args = static_cast<SwapReclaimArgs*>(kernel::GenericSlabAllocator::alloc(sizeof(SwapReclaimArgs)));
        if (!args) {
            // 슬랩 고갈 - 방금 세운 모든 rmap 엔트리를 그 자리에서
            // 원복한다(이 프레임을 영원히 회수 진행중 상태로 방치하는
            // 것보다 안전).
            for (kernel::RmapEntry* rmap = cursor->rmapHead; rmap; rmap = rmap->next) {
                kernel::Paging::cancelReclaimInProgress(rmap->virtAddr, rmap->owner->pml4Phys);
            }
            backend->freeSlot(slot);
            cursor = next;
            continue;
        }
        new (args) SwapReclaimArgs();
        args->device = backend->device();
        args->slot = slot;
        args->frame = cursor;
        args->physAddr = physAddr;

        kLruUnlink(gInactiveList, cursor);
        cursor->flags = static_cast<kernel::uint16_t>(cursor->flags | kernel::kPageFrameFlagReclaiming);

        const kernel::AsyncTaskSubjectCode subjectCode = kEnsureSwapReclaimWriteHandlerRegistered();
        kernel::AsyncTask* task = kernel::AsyncTask::submit(subjectCode, 0, args, /*autoFree=*/true);
        if (!task) {
            args->~SwapReclaimArgs();
            kernel::GenericSlabAllocator::free(args, sizeof(SwapReclaimArgs));
            for (kernel::RmapEntry* rmap = cursor->rmapHead; rmap; rmap = rmap->next) {
                kernel::Paging::cancelReclaimInProgress(rmap->virtAddr, rmap->owner->pml4Phys);
            }
            cursor->flags = static_cast<kernel::uint16_t>(cursor->flags & ~kernel::kPageFrameFlagReclaiming);
            kLruPushBack(gInactiveList, cursor);
            backend->freeSlot(slot);
        }
        cursor = next;
    }
}

// `kernel::DelayedExecutionQueue::schedule()`이 요구하는 원시 함수
// 포인터 시그니처(`void(*)(void*)`) - 매 실행 끝에 스스로를 다시
// 등록해 주기적 동작을 흉내낸다(전용 Task 없음, SP-F15B4A63 §3).
// arg는 안 씀(항상 nullptr로 등록).
void kReclaimScanCallback(void*) {
    {
        kernel::SpinlockGuard guard(gLruLock);
        kReclaimScanPromotePass();
        kReclaimScanDemotePass();
        // [보류, 2026-09-23, PN-4859FDE9 §7.2 5단계 - 실측으로 확정된
        // 행 버그 발견, 활성화 보류] 아래 kReclaimScanReclaimPass()/
        // SwapReclaimWriteHandler 자체(실제 스왑아웃 쓰기)는 실측으로
        // 완전히 검증됐다 - mkswap 실제 이미지+실제 init/pubreg/authmgr
        // 프로세스로 부팅해 5분+ 동안 58개 프레임을 실제로 스왑아웃
        // 시켰고 전부 성공(ok=1, 손상/크래시 없음). 문제는 그 반대편,
        // §4.2 스왑인(Paging::handlePageFault의 PAGE_SWAP_MARKER 분기 +
        // SwapInReadHandler + Scheduler::parkFromISR)에서 발견됐다 -
        // 스왑아웃된 프로세스가 그 페이지를 다시 건드려 실제로 폴트가
        // 나는 순간(실측 재현: va=0x400000, slot=54, 매 실행 100%
        // 재현) `Paging::handlePageFault`가 `kTrySubmitSwapIn()`까지
        // 정확히 실행하고(로그로 확인) `idt.cpp`가 `Scheduler::
        // parkFromISR(thread, frame)`를 부르는 지점까지도 도달하는데
        // (그 직전 로그도 정상 출력됨 - frame 내용 자체는 정상,
        // rip=0x400072/cs=0x23로 멀쩡한 ring3 코드 폴트), 그 뒤로는
        // 커널 전체가 조용히 완전히 멎는다(패닉 로그 없음, 이후 어떤
        // reclaim tick도, 어떤 로그도 다시 안 찍힘 - 재부팅도 안 되는
        // -no-reboot 상태의 순수 정지, SMP1). #DB의
        // kHandleUserBreakpointHit()이 정확히 같은 parkFromISR 패턴을
        // 이미 프로덕션에서 검증받았음에도 #PF에서는 재현되는 걸 보아
        // 두 IST 벡터(#DB=IST4/#PF=IST5) 사이에 아직 못 찾은 미묘한
        // 차이가 있는 것으로 보인다. gdb로 실제 정지 지점을 잡으려는
        // 시도는 이 정지 자체가 gdb 부착 시 타이밍이 크게 달라져(이
        // 프로젝트에 이미 여러 번 기록된 heisenbug 패턴, PN-3DDF2797/
        // PN-E4C6AF72와 동일 계열) 재현 자체가 극도로 느려지거나
        // 재현되지 않아 결론을 못 냈다 - 다음 세션은 게스트 내부
        // 저오버헤드 진단(진단 링 버퍼류, 이 프로젝트가 PN-E4C6AF72에서
        // 이미 채택한 방식)으로 접근할 것을 권장한다.
        //
        // **읽기(스왑인) 없이 쓰기(스왑아웃)만 활성화하면 절대 안
        // 된다**(이 계획 본문이 처음부터 명시한 제약 그대로 - 회수된
        // 페이지를 다시 건드리는 순간 이 행 버그로 시스템 전체가
        // 멎는다) - 그래서 위 §7.2 2/3/4단계(접근 감지+승격/강등)만
        // 남기고 5단계(실제 회수) 자체를 호출하지 않는다. 관련 코드
        // (kReclaimScanReclaimPass/SwapReclaimWriteHandler/
        // Paging::markReclaimInProgress 등 4개 신규 메서드/
        // kTrySubmitSwapIn/SwapInReadHandler/PAGE_RECLAIM_INPROGRESS/
        // Paging::PageFaultOutcome::ParkForSwapIn)는 전부 컴파일된 채로
        // 남겨 뒀다 - 다음 세션이 parkFromISR 버그만 고치면 이 줄
        // 하나(kReclaimScanReclaimPass() 호출)만 다시 살리면 된다.
        // kReclaimScanReclaimPass();
    }
    kernel::DelayedExecutionQueue::schedule(kReclaimScanIntervalTicks, &kReclaimScanCallback, nullptr);
}

}  // namespace

namespace kernel {

void PageFrameAllocator::init(const HvmMemmapEntry* memmap, uint32_t entryCount,
                               uint64_t kernelPhysStart, uint64_t kernelPhysEnd,
                               uint64_t startInfoAddr, uint64_t startInfoSize) {
    gNodeCount = Acpi::numaNodeCount();
    if (gNodeCount == 0) {
        gNodeCount = 1;
    }
    if (gNodeCount > kPfaMaxNumaNodes) {
        gNodeCount = kPfaMaxNumaNodes;
    }

    Range ranges[kMaxRanges];
    int count = 0;

    // Paging::init()이 이미 이 호출보다 먼저 실행돼 확정해 둔 실제
    // direct map 범위 - 그 이상은 direct map으로 볼 수 없는 물리
    // 프레임이라 애초에 내줄 수 없다(PL-99562483/PN-4AA5425D).
    const uint64_t mappedLimit = Paging::directMapLimit();

    for (uint32_t i = 0; i < entryCount; ++i) {
        if (memmap[i].type != static_cast<uint32_t>(HvmMemmapType::kUsable)) {
            continue;
        }
        uint64_t start = memmap[i].addr;
        uint64_t end = start + memmap[i].size;
        if (end > mappedLimit) {
            end = mappedLimit;
        }
        if (start >= end) {
            continue;
        }
        if (count < kMaxRanges) {
            ranges[count++] = {start, end};
        }
    }

    const auto memmapArrayAddr = reinterpret_cast<uint64_t>(memmap);
    const auto memmapArrayEnd = memmapArrayAddr + static_cast<uint64_t>(entryCount) * sizeof(HvmMemmapEntry);

    // [수정, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §1] `PageFrame` 배열을
    // 커널 이미지 바로 뒤에 bump 예약 - 기존 COW 참조 카운트
    // uint16_t 배열(SP-6BEAE0C1 §11-3)과 정확히 같은 배치 패턴,
    // 원소 크기만 64바이트로 커졌다. 프레임 개수는 실측한 direct map
    // 범위(mappedLimit) 기준이라 커널마다/부팅마다 크기가 다를 수
    // 있다.
    gPageFrameCount = mappedLimit / kPageSize;
    const uint64_t pageFrameArrayBytes = gPageFrameCount * sizeof(PageFrame);
    const uint64_t pageFrameArrayStart = kAlignUp(kernelPhysEnd, kPageSize);
    const uint64_t pageFrameArrayEnd = kAlignUp(pageFrameArrayStart + pageFrameArrayBytes, kPageSize);

    kSubtractReservedFromList(ranges, count, 0, kLowReservedEnd);
    kSubtractReservedFromList(ranges, count, kernelPhysStart, kernelPhysEnd);
    kSubtractReservedFromList(ranges, count, pageFrameArrayStart, pageFrameArrayEnd);
    kSubtractReservedFromList(ranges, count, startInfoAddr, startInfoAddr + startInfoSize);
    kSubtractReservedFromList(ranges, count, memmapArrayAddr, memmapArrayEnd);

    gPageFrames = reinterpret_cast<PageFrame*>(kPhysToVirt(pageFrameArrayStart));
    // memset(0)이면 충분하다 - PageFrame의 모든 필드가 0/nullptr을
    // "추적 안 됨/미배선 상태"로 삼는 NSDMI와 정확히 같은 값이라(이
    // 프로젝트 전역의 "전부 0 = 아직 실제 생성자를 부른 적 없는
    // 상태"와 동일한 관례, Process::allocate()의 memset(0)과 같은
    // 근거), 필드별로 개별 초기화할 필요가 없다.
    for (uint64_t i = 0; i < gPageFrameCount; ++i) {
        gPageFrames[i] = PageFrame{};
    }

    // [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §2] PG_RESERVED -
    // usable range에서 방금 빼낸(kSubtractReservedFromList) 다섯
    // 구간 전부가 "할당 대상 아님"이므로, 그 프레임들에도 실제로
    // 표시해 둔다(지금까지는 버디 트리에 안 들어간다는 사실 자체로만
    // "예약됨"을 표현했으나, PageFrame이 생긴 이상 그 사실을 플래그로도
    // 조회 가능하게 한다 - 새 소비자는 아직 없음, 순수 정보 제공).
    kSetPageFrameFlags(0, kLowReservedEnd, kPageFrameFlagReserved);
    kSetPageFrameFlags(kernelPhysStart, kernelPhysEnd, kPageFrameFlagReserved);
    kSetPageFrameFlags(pageFrameArrayStart, pageFrameArrayEnd, kPageFrameFlagReserved);
    kSetPageFrameFlags(startInfoAddr, startInfoAddr + startInfoSize, kPageFrameFlagReserved);
    kSetPageFrameFlags(memmapArrayAddr, memmapArrayEnd, kPageFrameFlagReserved);

    const bool haveAffinityInfo = Acpi::memoryAffinityCount() > 0;
    for (int i = 0; i < count; ++i) {
        if (ranges[i].start >= ranges[i].end) {
            continue;
        }
        if (haveAffinityInfo) {
            kPartitionRangeByAffinity(ranges[i].start, ranges[i].end);
        } else {
            kAssignRangeToNode(ranges[i].start, ranges[i].end, 0);
        }
    }
}

uint64_t PageFrameAllocator::allocOrderOnNode(uint32_t node, uint32_t order) {
    if (node >= gNodeCount) {
        return 0;
    }
    PfaNode& n = gNodes[node];
    SpinlockGuard guard(n.lock);
    const uint64_t addr = kObtainBlock(n, order);
    if (addr) {
        n.freePageCount -= (1UL << order);
    }
    return addr;
}

uint64_t PageFrameAllocator::allocOrderBelow(uint64_t physLimit, uint32_t order) {
    if (order > kMaxOrder) {
        return 0;
    }
    const uint64_t blockSize = kPageSize << order;
    for (uint32_t node = 0; node < gNodeCount; ++node) {
        PfaNode& n = gNodes[node];
        SpinlockGuard guard(n.lock);
        uint64_t cur = n.freeListHeads[order];
        while (cur) {
            const uint64_t next = kAsBlock(cur)->next;
            if (cur + blockSize <= physLimit) {
                kTryRemoveBlock(n, cur, order);  // 방금 이 리스트에서 찾은 값이라 항상 성공
                n.freePageCount -= (1UL << order);
                return cur;
            }
            cur = next;
        }
    }
    return 0;
}

uint64_t PageFrameAllocator::allocOrder(uint32_t order) {
    const uint32_t preferredNode = kCurrentNumaNode();
    uint64_t addr = allocOrderOnNode(preferredNode, order);
    if (addr) {
        return addr;
    }
    // 선호 노드에 없으면 다른 노드를 순서대로 뒤진다(NUMA 지역성보다
    // 할당 성공이 우선 - v1은 그 이상의 정책이 없다).
    for (uint32_t node = 0; node < gNodeCount; ++node) {
        if (node == preferredNode) {
            continue;
        }
        addr = allocOrderOnNode(node, order);
        if (addr) {
            return addr;
        }
    }
    return 0;
}

void PageFrameAllocator::retain(uint64_t physAddr) {
    const uint64_t idx = physAddr / kPageSize;
    if (!gPageFrames || idx >= gPageFrameCount) {
        return;  // 방어적 - direct map 밖 주소는 애초에 이 할당자가 준 적 없음
    }
    uint16_t& count = gPageFrames[idx].refCount;
    count = (count == 0) ? 2 : static_cast<uint16_t>(count + 1);
}

uint32_t PageFrameAllocator::refCount(uint64_t physAddr) {
    const uint64_t idx = physAddr / kPageSize;
    if (!gPageFrames || idx >= gPageFrameCount) {
        return 0;
    }
    return gPageFrames[idx].refCount;
}

PageFrame* PageFrameAllocator::frameFor(uint64_t physAddr) {
    const uint64_t idx = physAddr / kPageSize;
    if (!gPageFrames || idx >= gPageFrameCount) {
        return nullptr;
    }
    return &gPageFrames[idx];
}

bool PageFrameAllocator::insertRmap(uint64_t physAddr, Process* owner, uint64_t virtAddr) {
    PageFrame* frame = frameFor(physAddr);
    if (!frame) {
        return false;
    }
    auto* entry = static_cast<RmapEntry*>(GenericSlabAllocator::alloc(sizeof(RmapEntry)));
    if (!entry) {
        return false;
    }
    *entry = RmapEntry{};
    entry->owner = owner;
    entry->virtAddr = virtAddr;

    SpinlockGuard guard(gLruLock);
    entry->next = frame->rmapHead;
    frame->rmapHead = entry;
    frame->mapCount = static_cast<uint16_t>(frame->mapCount + 1);
    // SP-6CEFBE9B §7.2 1단계 - 이 프레임이 처음 스왑 추적 대상이 되는
    // 순간(PG_SWAPPABLE이 꺼져 있던 상태)에만 inactive 리스트에 넣는다 -
    // COW 등으로 두 번째 이상 rmap 엔트리가 붙는 경우는 이미 리스트에
    // 있으므로 다시 넣지 않는다.
    if (!(frame->flags & kPageFrameFlagSwappable)) {
        frame->flags |= kPageFrameFlagSwappable;
        kLruPushBack(gInactiveList, frame);
    }
    return true;
}

void PageFrameAllocator::removeRmap(uint64_t physAddr, Process* owner, uint64_t virtAddr) {
    PageFrame* frame = frameFor(physAddr);
    if (!frame) {
        return;
    }
    SpinlockGuard guard(gLruLock);
    RmapEntry** cur = &frame->rmapHead;
    while (*cur) {
        if ((*cur)->owner == owner && (*cur)->virtAddr == virtAddr) {
            RmapEntry* dead = *cur;
            *cur = dead->next;
            GenericSlabAllocator::free(dead, sizeof(RmapEntry));
            if (frame->mapCount > 0) {
                frame->mapCount = static_cast<uint16_t>(frame->mapCount - 1);
            }
            return;
        }
        cur = &(*cur)->next;
    }
}

void PageFrameAllocator::cancelReclaimForFrame(PageFrame* frame) {
    SpinlockGuard guard(gLruLock);
    if (!(frame->flags & kPageFrameFlagReclaiming) || (frame->flags & kPageFrameFlagReclaimCanceled)) {
        return;  // 회수 대상이 아니거나 이미 취소됨(레이스로 두 번 불릴 수 있음) - 방어적
    }
    frame->flags |= kPageFrameFlagReclaimCanceled;
    for (RmapEntry* rmap = frame->rmapHead; rmap; rmap = rmap->next) {
        Paging::cancelReclaimInProgress(rmap->virtAddr, rmap->owner->pml4Phys);
    }
}

void PageFrameAllocator::freeOrder(uint64_t physAddr, uint32_t order) {
    // COW로 공유된 적 있는 4KiB 페이지(order 0)는 카운트가 0으로
    // 돌아올 때까지 실제로 반납하지 않는다 - retain()을 한 번도 안
    // 받은 페이지는 항상 0이라 기존 호출부 전부 이 분기를 그대로
    // 지나쳐 원래 동작과 동일하다(SP-6BEAE0C1 §11-3).
    if (order == 0 && gPageFrames) {
        const uint64_t idx = physAddr / kPageSize;
        if (idx < gPageFrameCount && gPageFrames[idx].refCount != 0) {
            if (--gPageFrames[idx].refCount != 0) {
                return;
            }
        }
    }
    // [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §6.2/§7.2] 이 프레임이
    // 정말로 버디 free list에 되돌아가기 직전 - rmap/LRU 소속을 전부
    // 청소한다. 정상 경로라면 `removeRmap()`이 호출부(unmapRegion 등)
    // 에서 이미 rmapHead를 비웠어야 하지만, 방어적 마지막 안전망으로
    // 남은 엔트리가 있으면 여기서 강제로 반납한다 - 이 프레임은 이제
    // 완전히 다른 매핑으로 재사용될 수 있어 낡은 rmap을 남기면 다음
    // 소유자의 페이지 폴트 처리에서 엉뚱한 프로세스를 가리키게 된다.
    if (order == 0) {
        if (PageFrame* frame = frameFor(physAddr)) {
            SpinlockGuard guard(gLruLock);
            if (frame->flags & kPageFrameFlagSwappable) {
                kLruUnlink(frame->flags & kPageFrameFlagActive ? gActiveList : gInactiveList, frame);
            }
            RmapEntry* entry = frame->rmapHead;
            while (entry) {
                RmapEntry* next = entry->next;
                GenericSlabAllocator::free(entry, sizeof(RmapEntry));
                entry = next;
            }
            frame->rmapHead = nullptr;
            frame->mapCount = 0;
            frame->flags &= static_cast<uint16_t>(
                ~(kPageFrameFlagSwappable | kPageFrameFlagActive | kPageFrameFlagAccessed));
        }
    }
    const uint32_t node = kNodeForAddress(physAddr);
    PfaNode& n = gNodes[node];
    SpinlockGuard guard(n.lock);
    n.freePageCount += (1UL << order);
    while (order < kMaxOrder) {
        const uint64_t buddy = kBuddyAddr(physAddr, order);
        if (!kTryRemoveBlock(n, buddy, order)) {
            break;
        }
        physAddr = physAddr < buddy ? physAddr : buddy;
        ++order;
    }
    kInsertBlock(n, physAddr, order);
}

uint64_t PageFrameAllocator::allocPage() {
    return allocOrder(0);
}

uint64_t PageFrameAllocator::allocPageOnNode(uint32_t node) {
    return allocOrderOnNode(node, 0);
}

void PageFrameAllocator::freePage(uint64_t physAddr) {
    freeOrder(physAddr, 0);
}

uint64_t PageFrameAllocator::freePageCount() {
    uint64_t total = 0;
    for (uint32_t i = 0; i < gNodeCount; ++i) {
        total += gNodes[i].freePageCount;
    }
    return total;
}

uint32_t PageFrameAllocator::numaNodeCount() {
    return gNodeCount;
}

uint64_t PageFrameAllocator::freePageCountOnNode(uint32_t node) {
    return node < gNodeCount ? gNodes[node].freePageCount : 0;
}

void PageFrameAllocator::startReclaimScan() {
    DelayedExecutionQueue::schedule(kReclaimScanIntervalTicks, &kReclaimScanCallback, nullptr);
}

}  // namespace kernel
