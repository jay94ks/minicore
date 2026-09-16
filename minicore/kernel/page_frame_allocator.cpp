#include "page_frame_allocator.h"

#include "acpi.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "paging.h"

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

struct Node {
    kernel::uint64_t freeListHeads[kMaxOrder + 1];  // 물리주소, 0 = 비어있음
    kernel::uint64_t freePageCount;
    // 이 노드의 free list를 건드리는 공개 API(allocOrderOnNode/
    // freeOrder) 진입점 하나당 한 번만 잠근다 - kObtainBlock의 내부
    // 재귀는 이미 잠긴 상태로 도는 거라 다시 잠그지 않는다(SMP 0단계,
    // QU-B97FDA44, 2026-09-14).
    kernel::Spinlock lock;
};

Node gNodes[kernel::kPfaMaxNumaNodes];
kernel::uint32_t gNodeCount = 1;

// [SP-6BEAE0C1 §11-3] COW 공유 카운트 배열 - 프레임 개수는 실행 중
// 실측한 Paging::directMapLimit()(4~512GiB 동적)에 맞춰 init()에서
// 정해지므로 정적 배열이 아니라 커널 이미지 바로 뒤 물리 공간을
// bump 방식으로 예약해 둔다(직접 매핑 범위 안이라 별도 매핑 없이
// kPhysToVirt로 바로 접근 가능 - Paging::init()이 이 호출보다 먼저
// 전체 direct map을 이미 확정해 둠).
kernel::uint16_t* gCowRefCounts = nullptr;
kernel::uint64_t gCowRefCountFrames = 0;

kernel::uint64_t kAlignUp(kernel::uint64_t value, kernel::uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

kernel::uint64_t kAlignDown(kernel::uint64_t value, kernel::uint64_t align) {
    return value & ~(align - 1);
}

FreeBlock* kAsBlock(kernel::uint64_t physAddr) {
    return reinterpret_cast<FreeBlock*>(kernel::kPhysToVirt(physAddr));
}

void kInsertBlock(Node& node, kernel::uint64_t addr, kernel::uint32_t order) {
    kAsBlock(addr)->next = node.freeListHeads[order];
    node.freeListHeads[order] = addr;
}

bool kTryRemoveBlock(Node& node, kernel::uint64_t addr, kernel::uint32_t order) {
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

kernel::uint64_t kPopBlock(Node& node, kernel::uint32_t order) {
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
kernel::uint64_t kObtainBlock(Node& node, kernel::uint32_t order) {
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

void kAddRegionToBuddy(Node& node, kernel::uint64_t start, kernel::uint64_t end) {
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

void kAssignRangeToNode(kernel::uint64_t start, kernel::uint64_t end, kernel::uint32_t node) {
    if (start >= end) {
        return;
    }
    if (node >= gNodeCount) {
        node = 0;  // 방어적 fallback
    }
    kAddRegionToBuddy(gNodes[node], start, end);
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

    // [SP-6BEAE0C1 §11-3] COW 참조 카운트 배열을 커널 이미지 바로 뒤에
    // bump 예약 - 프레임 개수는 실측한 direct map 범위(mappedLimit)
    // 기준이라 커널마다/부팅마다 크기가 다를 수 있다.
    gCowRefCountFrames = mappedLimit / kPageSize;
    const uint64_t cowArrayBytes = gCowRefCountFrames * sizeof(uint16_t);
    const uint64_t cowArrayStart = kAlignUp(kernelPhysEnd, kPageSize);
    const uint64_t cowArrayEnd = kAlignUp(cowArrayStart + cowArrayBytes, kPageSize);

    kSubtractReservedFromList(ranges, count, 0, kLowReservedEnd);
    kSubtractReservedFromList(ranges, count, kernelPhysStart, kernelPhysEnd);
    kSubtractReservedFromList(ranges, count, cowArrayStart, cowArrayEnd);
    kSubtractReservedFromList(ranges, count, startInfoAddr, startInfoAddr + startInfoSize);
    kSubtractReservedFromList(ranges, count, memmapArrayAddr, memmapArrayEnd);

    gCowRefCounts = reinterpret_cast<uint16_t*>(kPhysToVirt(cowArrayStart));
    for (uint64_t i = 0; i < gCowRefCountFrames; ++i) {
        gCowRefCounts[i] = 0;
    }

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
    Node& n = gNodes[node];
    SpinlockGuard guard(n.lock);
    const uint64_t addr = kObtainBlock(n, order);
    if (addr) {
        n.freePageCount -= (1UL << order);
    }
    return addr;
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
    if (!gCowRefCounts || idx >= gCowRefCountFrames) {
        return;  // 방어적 - direct map 밖 주소는 애초에 이 할당자가 준 적 없음
    }
    uint16_t& count = gCowRefCounts[idx];
    count = (count == 0) ? 2 : static_cast<uint16_t>(count + 1);
}

uint32_t PageFrameAllocator::refCount(uint64_t physAddr) {
    const uint64_t idx = physAddr / kPageSize;
    if (!gCowRefCounts || idx >= gCowRefCountFrames) {
        return 0;
    }
    return gCowRefCounts[idx];
}

void PageFrameAllocator::freeOrder(uint64_t physAddr, uint32_t order) {
    // COW로 공유된 적 있는 4KiB 페이지(order 0)는 카운트가 0으로
    // 돌아올 때까지 실제로 반납하지 않는다 - retain()을 한 번도 안
    // 받은 페이지는 항상 0이라 기존 호출부 전부 이 분기를 그대로
    // 지나쳐 원래 동작과 동일하다(SP-6BEAE0C1 §11-3).
    if (order == 0 && gCowRefCounts) {
        const uint64_t idx = physAddr / kPageSize;
        if (idx < gCowRefCountFrames && gCowRefCounts[idx] != 0) {
            if (--gCowRefCounts[idx] != 0) {
                return;
            }
        }
    }
    const uint32_t node = kNodeForAddress(physAddr);
    Node& n = gNodes[node];
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

}  // namespace kernel
