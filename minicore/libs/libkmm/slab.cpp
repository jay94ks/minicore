#include "slab.h"

#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "scheduler.h"

namespace kernel {

SlabCache::MagazineNode* SlabCache::popDepot(AtomicPtr<MagazineNode>& stack) {
    MagazineNode* head = stack.load();
    while (head) {
        MagazineNode* next = head->next;
        if (stack.compareExchange(head, next)) {
            return head;
        }
        // 실패하면 head가 현재 값으로 갱신돼 있으니 그대로 재시도.
    }
    return nullptr;
}

void SlabCache::pushDepot(AtomicPtr<MagazineNode>& stack, MagazineNode* node) {
    MagazineNode* head = stack.load();
    do {
        node->next = head;
    } while (!stack.compareExchange(head, node));
}

void SlabCache::init(unsigned long objectSize) {
    _objectSize = objectSize;
    // 노드 풀 2*kMaxCores개를 코어별 loaded/previous 슬롯에 정확히
    // 나눠준다 - 디포는 처음엔 비어 있고(양쪽 다), alloc()/free()가
    // "빈/가득 찬 매거진을 서로 주고받는" 과정에서 자연스럽게 채워
    // 진다. 총 노드 개수는 런타임 내내 불변이다(새로 만들거나 없애지
    // 않음 - 코어<->디포 사이를 옮겨 다니기만 함).
    unsigned int index = 0;
    for (unsigned int core = 0; core < kMaxCores; ++core) {
        _perCore[core].loaded = &_nodePool[index++];
        _perCore[core].previous = &_nodePool[index++];
    }
}

void* SlabCache::alloc() {
    PreemptionGuard guard;
    const unsigned int core = Scheduler::currentCoreIndex();
    CoreState& cs = _perCore[core];

    if (cs.loaded->count > 0) {
        return cs.loaded->items[--cs.loaded->count];
    }
    if (cs.previous->count > 0) {
        MagazineNode* tmp = cs.loaded;
        cs.loaded = cs.previous;
        cs.previous = tmp;
        return cs.loaded->items[--cs.loaded->count];
    }

    // 코어 매거진 둘 다 빔 - 디포에서 가득 찬 매거진을 받아온다.
    MagazineNode* full = popDepot(_fullDepot);
    if (full) {
        // 지금 비어 있는 previous를 디포(빈 목록)에 돌려주고, 대신
        // 받은 가득 찬 매거진을 previous 자리에 앉힌 뒤 swap한다 -
        // free()의 대칭 연산과 정확히 반대 방향.
        pushDepot(_emptyDepot, cs.previous);
        cs.previous = full;
        MagazineNode* tmp = cs.loaded;
        cs.loaded = cs.previous;
        cs.previous = tmp;
        return cs.loaded->items[--cs.loaded->count];
    }

    // 디포에도 없음 - 매거진 계층을 건너뛰고 원시 슬랩 페이지에서
    // 직접 하나 받아온다.
    return slabLayerAlloc();
}

void SlabCache::free(void* ptr) {
    PreemptionGuard guard;
    const unsigned int core = Scheduler::currentCoreIndex();
    CoreState& cs = _perCore[core];

    if (cs.loaded->count < kMagazineCapacity) {
        cs.loaded->items[cs.loaded->count++] = ptr;
        return;
    }
    if (cs.previous->count < kMagazineCapacity) {
        MagazineNode* tmp = cs.loaded;
        cs.loaded = cs.previous;
        cs.previous = tmp;
        cs.loaded->items[cs.loaded->count++] = ptr;
        return;
    }

    // 코어 매거진 둘 다 가득 참 - 디포에서 빈 매거진을 받아와야 한다.
    MagazineNode* empty = popDepot(_emptyDepot);
    if (!empty) {
        // 디포의 빈 매거진 예비분까지 없는 극단적 상황(이론상 가능
        // - 실사용에서는 alloc() 쪽이 계속 빈 매거진을 채워 넣으므로
        // 매우 드묾) - 매거진 계층을 거치지 않고 원시 슬랩 페이지
        // free-list에 바로 반납한다. 항상 안전한 폴백이다.
        slabLayerFree(ptr);
        return;
    }
    // 가득 찬 previous를 디포(가득 목록)에 돌려주고, 받은 빈 매거진을
    // previous 자리에 앉힌 뒤 swap한다.
    pushDepot(_fullDepot, cs.previous);
    cs.previous = empty;
    MagazineNode* tmp = cs.loaded;
    cs.loaded = cs.previous;
    cs.previous = tmp;
    cs.loaded->items[cs.loaded->count++] = ptr;
}

void SlabCache::growRawFreeList() {
    const uint64_t phys = PageFrameAllocator::allocPage();
    if (!phys) {
        return;
    }
    auto* base = reinterpret_cast<uint8_t*>(kPhysToVirt(phys));
    const unsigned long count = 4096UL / _objectSize;
    for (unsigned long i = 0; i < count; ++i) {
        void* obj = base + i * _objectSize;
        *reinterpret_cast<void**>(obj) = _rawFreeList;
        _rawFreeList = obj;
    }
}

void* SlabCache::slabLayerAlloc() {
    SpinlockGuard guard(_rawLock);
    if (!_rawFreeList) {
        growRawFreeList();
    }
    if (!_rawFreeList) {
        return nullptr;  // PageFrameAllocator까지 완전히 고갈 - 비블로킹 실패
    }
    void* obj = _rawFreeList;
    _rawFreeList = *reinterpret_cast<void**>(obj);
    return obj;
}

void SlabCache::slabLayerFree(void* ptr) {
    SpinlockGuard guard(_rawLock);
    *reinterpret_cast<void**>(ptr) = _rawFreeList;
    _rawFreeList = ptr;
}

namespace {

constexpr unsigned long kBucketSizes[7] = {32, 64, 128, 256, 512, 1024, 2048};
constexpr int kBucketCount = 7;

int kBucketIndex(unsigned long bucket) {
    for (int i = 0; i < kBucketCount; ++i) {
        if (kBucketSizes[i] == bucket) {
            return i;
        }
    }
    return -1;
}

// 바이트 크기를 담기에 충분한 최소 버디 order(4KiB 단위) - task.cpp의
// kOrderForStackSize와 같은 계산.
uint32_t kOrderForByteSize(uint64_t size) {
    const uint64_t pages = (size + 4095UL) / 4096UL;
    uint32_t order = 0;
    while ((1UL << order) < pages) {
        ++order;
    }
    return order;
}

SlabCache gCaches[kBucketCount];

}  // namespace

unsigned long sizeToBucket(unsigned long size) {
    for (unsigned long bucket : kBucketSizes) {
        if (size <= bucket) {
            return bucket;
        }
    }
    return 0;
}

void GenericSlabAllocator::init() {
    for (int i = 0; i < kBucketCount; ++i) {
        gCaches[i].init(kBucketSizes[i]);
    }
}

void* GenericSlabAllocator::alloc(unsigned long size) {
    const unsigned long bucket = sizeToBucket(size);
    if (bucket == 0) {
        // 2048B 초과 - 슬랩을 거치지 않고 PageFrameAllocator로 직접
        // 위임한다(§2.3 - "대형 객체를 굳이 버킷화하지 않음").
        const uint32_t order = kOrderForByteSize(size);
        const uint64_t phys = PageFrameAllocator::allocOrder(order);
        return phys ? reinterpret_cast<void*>(kPhysToVirt(phys)) : nullptr;
    }
    return gCaches[kBucketIndex(bucket)].alloc();
}

void GenericSlabAllocator::free(void* ptr, unsigned long size) {
    if (!ptr) {
        return;
    }
    const unsigned long bucket = sizeToBucket(size);
    if (bucket == 0) {
        const uint32_t order = kOrderForByteSize(size);
        PageFrameAllocator::freeOrder(kVirtToPhys(reinterpret_cast<uint64_t>(ptr)), order);
        return;
    }
    gCaches[kBucketIndex(bucket)].free(ptr);
}

}  // namespace kernel
