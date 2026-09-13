#include "page_frame_allocator.h"

#include "acpi.h"
#include "lapic.h"

namespace {

constexpr unsigned long kPageSize = 4096;
constexpr unsigned int kMaxOrder = 10;  // 4KiB << 10 = 4MiB 최대 블록
// boot.S가 identity/higher-half로 정적 매핑해 둔 범위와 반드시 맞춰야
// 한다(관계도 기록됨) - 그 밖의 물리 메모리는 아직 이 v1에서 안 다룬다.
constexpr unsigned long kMappedLimit = 1UL << 30;        // 1GiB
constexpr unsigned long kLowReservedEnd = 0x200000;      // 2MiB: BIOS 영역 + 커널 자신

struct FreeBlock {
    FreeBlock* next;
};

struct Node {
    FreeBlock* freeLists[kMaxOrder + 1];
    unsigned long freePageCount;
};

Node gNodes[kernel::kPfaMaxNumaNodes];
unsigned int gNodeCount = 1;

unsigned long kAlignUp(unsigned long value, unsigned long align) {
    return (value + align - 1) & ~(align - 1);
}

unsigned long kAlignDown(unsigned long value, unsigned long align) {
    return value & ~(align - 1);
}

void kInsertBlock(Node& node, unsigned long addr, unsigned int order) {
    auto* block = reinterpret_cast<FreeBlock*>(addr);
    block->next = node.freeLists[order];
    node.freeLists[order] = block;
}

bool kTryRemoveBlock(Node& node, unsigned long addr, unsigned int order) {
    FreeBlock** cur = &node.freeLists[order];
    while (*cur) {
        if (reinterpret_cast<unsigned long>(*cur) == addr) {
            *cur = (*cur)->next;
            return true;
        }
        cur = &(*cur)->next;
    }
    return false;
}

unsigned long kPopBlock(Node& node, unsigned int order) {
    FreeBlock* block = node.freeLists[order];
    if (!block) {
        return 0;
    }
    node.freeLists[order] = block->next;
    return reinterpret_cast<unsigned long>(block);
}

unsigned long kBuddyAddr(unsigned long addr, unsigned int order) {
    return addr ^ (kPageSize << order);
}

// 지정한 order의 블록을 하나 확보한다 - 없으면 한 단계 큰 블록을
// 재귀적으로 얻어 반으로 쪼개고(짝 하나는 그 order 리스트에 도로
// 넣음), 전체 free 카운트는 여기서 건드리지 않는다(쪼개도 총량은
// 그대로라서 - 카운트 조정은 공개 API에서 한 번만 한다).
unsigned long kObtainBlock(Node& node, unsigned int order) {
    if (order > kMaxOrder) {
        return 0;
    }
    unsigned long addr = kPopBlock(node, order);
    if (addr) {
        return addr;
    }
    unsigned long bigger = kObtainBlock(node, order + 1);
    if (!bigger) {
        return 0;
    }
    unsigned long buddy = bigger + (kPageSize << order);
    kInsertBlock(node, buddy, order);
    return bigger;
}

void kAddRegionToBuddy(Node& node, unsigned long start, unsigned long end) {
    start = kAlignUp(start, kPageSize);
    end = kAlignDown(end, kPageSize);
    while (start < end) {
        unsigned int order = kMaxOrder;
        while (order > 0) {
            const unsigned long blockSize = kPageSize << order;
            if ((start % blockSize) == 0 && (start + blockSize) <= end) {
                break;
            }
            --order;
        }
        const unsigned long blockSize = kPageSize << order;
        kInsertBlock(node, start, order);
        node.freePageCount += (1UL << order);
        start += blockSize;
    }
}

struct Range {
    unsigned long start;
    unsigned long end;
};

constexpr int kMaxRanges = 64;

// ranges[0..count)에서 [resStart, resEnd)와 겹치는 부분을 전부
// 잘라낸다 - 겹치는 range는 앞쪽 조각으로 축소(또는 완전히 없어짐)
// 되고, 뒤쪽 조각이 남으면 목록 끝에 새로 추가한다.
void kSubtractReservedFromList(Range* ranges, int& count, unsigned long resStart, unsigned long resEnd) {
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
    unsigned long start;
    unsigned long end;
    unsigned int node;
};
constexpr int kMaxRangeNodes = 128;
RangeNode gRangeNodeMap[kMaxRangeNodes];
int gRangeNodeMapCount = 0;

void kAssignRangeToNode(unsigned long start, unsigned long end, unsigned int node) {
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
unsigned int kCurrentNumaNode() {
    if (!kernel::Lapic::isReady()) {
        return 0;
    }
    const unsigned int myApicId = kernel::Lapic::id();
    const unsigned int cpuCount = kernel::Acpi::cpuCount();
    for (unsigned int i = 0; i < cpuCount; ++i) {
        if (kernel::Acpi::cpuApicId(i) == myApicId) {
            return kernel::Acpi::cpuNumaNode(i);
        }
    }
    return 0;
}

unsigned int kNodeForAddress(unsigned long addr) {
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
void kPartitionRangeByAffinity(unsigned long rangeStart, unsigned long rangeEnd) {
    Range remaining[kMaxRanges];
    int remainingCount = 1;
    remaining[0] = {rangeStart, rangeEnd};

    const unsigned int affinityCount = kernel::Acpi::memoryAffinityCount();
    for (unsigned int a = 0; a < affinityCount && remainingCount > 0; ++a) {
        const unsigned long affBase = kernel::Acpi::memoryAffinityBase(a);
        const unsigned long affEnd = affBase + kernel::Acpi::memoryAffinityLength(a);
        const unsigned int affNode = kernel::Acpi::memoryAffinityNode(a);

        Range next[kMaxRanges];
        int nextCount = 0;
        for (int i = 0; i < remainingCount; ++i) {
            const unsigned long pStart = remaining[i].start;
            const unsigned long pEnd = remaining[i].end;
            const unsigned long ovStart = pStart > affBase ? pStart : affBase;
            const unsigned long ovEnd = pEnd < affEnd ? pEnd : affEnd;
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

void PageFrameAllocator::init(const HvmMemmapEntry* memmap, unsigned int entryCount,
                               unsigned long kernelPhysStart, unsigned long kernelPhysEnd,
                               unsigned long startInfoAddr, unsigned long startInfoSize) {
    gNodeCount = Acpi::numaNodeCount();
    if (gNodeCount == 0) {
        gNodeCount = 1;
    }
    if (gNodeCount > kPfaMaxNumaNodes) {
        gNodeCount = kPfaMaxNumaNodes;
    }

    Range ranges[kMaxRanges];
    int count = 0;

    for (unsigned int i = 0; i < entryCount; ++i) {
        if (memmap[i].type != static_cast<unsigned int>(HvmMemmapType::kUsable)) {
            continue;
        }
        unsigned long start = memmap[i].addr;
        unsigned long end = start + memmap[i].size;
        if (end > kMappedLimit) {
            end = kMappedLimit;
        }
        if (start >= end) {
            continue;
        }
        if (count < kMaxRanges) {
            ranges[count++] = {start, end};
        }
    }

    const auto memmapArrayAddr = reinterpret_cast<unsigned long>(memmap);
    const auto memmapArrayEnd = memmapArrayAddr + static_cast<unsigned long>(entryCount) * sizeof(HvmMemmapEntry);

    kSubtractReservedFromList(ranges, count, 0, kLowReservedEnd);
    kSubtractReservedFromList(ranges, count, kernelPhysStart, kernelPhysEnd);
    kSubtractReservedFromList(ranges, count, startInfoAddr, startInfoAddr + startInfoSize);
    kSubtractReservedFromList(ranges, count, memmapArrayAddr, memmapArrayEnd);

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

unsigned long PageFrameAllocator::allocOrderOnNode(unsigned int node, unsigned int order) {
    if (node >= gNodeCount) {
        return 0;
    }
    const unsigned long addr = kObtainBlock(gNodes[node], order);
    if (addr) {
        gNodes[node].freePageCount -= (1UL << order);
    }
    return addr;
}

unsigned long PageFrameAllocator::allocOrder(unsigned int order) {
    const unsigned int preferredNode = kCurrentNumaNode();
    unsigned long addr = allocOrderOnNode(preferredNode, order);
    if (addr) {
        return addr;
    }
    // 선호 노드에 없으면 다른 노드를 순서대로 뒤진다(NUMA 지역성보다
    // 할당 성공이 우선 - v1은 그 이상의 정책이 없다).
    for (unsigned int node = 0; node < gNodeCount; ++node) {
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

void PageFrameAllocator::freeOrder(unsigned long physAddr, unsigned int order) {
    const unsigned int node = kNodeForAddress(physAddr);
    Node& n = gNodes[node];
    n.freePageCount += (1UL << order);
    while (order < kMaxOrder) {
        const unsigned long buddy = kBuddyAddr(physAddr, order);
        if (!kTryRemoveBlock(n, buddy, order)) {
            break;
        }
        physAddr = physAddr < buddy ? physAddr : buddy;
        ++order;
    }
    kInsertBlock(n, physAddr, order);
}

unsigned long PageFrameAllocator::allocPage() {
    return allocOrder(0);
}

unsigned long PageFrameAllocator::allocPageOnNode(unsigned int node) {
    return allocOrderOnNode(node, 0);
}

void PageFrameAllocator::freePage(unsigned long physAddr) {
    freeOrder(physAddr, 0);
}

unsigned long PageFrameAllocator::freePageCount() {
    unsigned long total = 0;
    for (unsigned int i = 0; i < gNodeCount; ++i) {
        total += gNodes[i].freePageCount;
    }
    return total;
}

unsigned int PageFrameAllocator::numaNodeCount() {
    return gNodeCount;
}

unsigned long PageFrameAllocator::freePageCountOnNode(unsigned int node) {
    return node < gNodeCount ? gNodes[node].freePageCount : 0;
}

}  // namespace kernel
