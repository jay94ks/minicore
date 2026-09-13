#include "page_frame_allocator.h"

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

FreeBlock* gFreeLists[kMaxOrder + 1] = {};
unsigned long gFreePageCount = 0;

unsigned long kAlignUp(unsigned long value, unsigned long align) {
    return (value + align - 1) & ~(align - 1);
}

unsigned long kAlignDown(unsigned long value, unsigned long align) {
    return value & ~(align - 1);
}

void kInsertBlock(unsigned long addr, unsigned int order) {
    auto* block = reinterpret_cast<FreeBlock*>(addr);
    block->next = gFreeLists[order];
    gFreeLists[order] = block;
}

bool kTryRemoveBlock(unsigned long addr, unsigned int order) {
    FreeBlock** cur = &gFreeLists[order];
    while (*cur) {
        if (reinterpret_cast<unsigned long>(*cur) == addr) {
            *cur = (*cur)->next;
            return true;
        }
        cur = &(*cur)->next;
    }
    return false;
}

unsigned long kPopBlock(unsigned int order) {
    FreeBlock* block = gFreeLists[order];
    if (!block) {
        return 0;
    }
    gFreeLists[order] = block->next;
    return reinterpret_cast<unsigned long>(block);
}

unsigned long kBuddyAddr(unsigned long addr, unsigned int order) {
    return addr ^ (kPageSize << order);
}

// 지정한 order의 블록을 하나 확보한다 - 없으면 한 단계 큰 블록을
// 재귀적으로 얻어 반으로 쪼개고(짝 하나는 그 order 리스트에 도로
// 넣음), 전체 free 카운트는 여기서 건드리지 않는다(쪼개도 총량은
// 그대로라서 - 카운트 조정은 공개 API에서 한 번만 한다).
unsigned long kObtainBlock(unsigned int order) {
    if (order > kMaxOrder) {
        return 0;
    }
    unsigned long addr = kPopBlock(order);
    if (addr) {
        return addr;
    }
    unsigned long bigger = kObtainBlock(order + 1);
    if (!bigger) {
        return 0;
    }
    unsigned long buddy = bigger + (kPageSize << order);
    kInsertBlock(buddy, order);
    return bigger;
}

void kAddRegionToBuddy(unsigned long start, unsigned long end) {
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
        kInsertBlock(start, order);
        gFreePageCount += (1UL << order);
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

}  // namespace

namespace kernel {

void PageFrameAllocator::kInit(const HvmMemmapEntry* memmap, unsigned int entryCount,
                                unsigned long kernelPhysStart, unsigned long kernelPhysEnd,
                                unsigned long startInfoAddr, unsigned long startInfoSize) {
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

    for (int i = 0; i < count; ++i) {
        if (ranges[i].start < ranges[i].end) {
            kAddRegionToBuddy(ranges[i].start, ranges[i].end);
        }
    }
}

unsigned long PageFrameAllocator::kAllocOrder(unsigned int order) {
    const unsigned long addr = kObtainBlock(order);
    if (addr) {
        gFreePageCount -= (1UL << order);
    }
    return addr;
}

void PageFrameAllocator::kFreeOrder(unsigned long physAddr, unsigned int order) {
    gFreePageCount += (1UL << order);
    while (order < kMaxOrder) {
        const unsigned long buddy = kBuddyAddr(physAddr, order);
        if (!kTryRemoveBlock(buddy, order)) {
            break;
        }
        physAddr = physAddr < buddy ? physAddr : buddy;
        ++order;
    }
    kInsertBlock(physAddr, order);
}

unsigned long PageFrameAllocator::kAllocPage() {
    return kAllocOrder(0);
}

void PageFrameAllocator::kFreePage(unsigned long physAddr) {
    kFreeOrder(physAddr, 0);
}

unsigned long PageFrameAllocator::kFreePageCount() {
    return gFreePageCount;
}

}  // namespace kernel
