#ifndef MINICORE_KERNEL_CONCURRENT_MAP_H
#define MINICORE_KERNEL_CONCURRENT_MAP_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "rcu.h"

// [배치 정정, 2026-09-17] SP-4DCD0E6A §3은 이 컨테이너를 `libkcont`
// (커널/유저 공용)에 두도록 스케치했으나, `lockfree_list.h`/
// `lockfree_queue.h`가 이미 겪은 것과 정확히 같은 이유로 여기(`minicore/
// kernel`)에 둔다 - 정확성이 RCU(`rcu.h`)에 의존하는데 RCU는 Scheduler/
// PerCpu 기반 순수 커널 개념이라 유저랜드 대응물이 없고, `libkcont`가
// 이 헤더를 담으면 계층 위반(및 컴파일 실패 - `libkcont`의 include
// 경로에 `rcu.h`가 없음)이다. 설계(알고리즘/API) 자체는 그대로, 파일
// 위치만 조정(구현 세부 판단, 별도 설계자 확인 불필요 - 이미 두 번
// 확립된 패턴을 세 번째로 따르는 것뿐).
//
// ConcurrentMap<K, V, BucketCount, LockStripes>(SP-4DCD0E6A §3,
// PN-A8EF29F7) - C# ConcurrentDictionary 패턴: 고정 개수 버킷(각자
// 단일 연결 리스트) + 버킷 인덱스를 고정 개수 Spinlock으로 나눠 잠그는
// 세분화된 락(striped lock). 쓰기(insert/remove)는 해당 스트라이프
// 락만 잡고, 읽기(find)는 완전히 락 없이(lock-free) RCU로 그 버킷
// 체인을 순회한다.
//
// **비침습(non-intrusive) 설계**: `LockFreeQueue`(PN-013215F9)가 이미
// 겪은 것과 같은 이유 - 이 맵은 임의의 K/V 타입을 저장해야 하므로
// 호출자가 자기 구조체에 노드를 미리 내장해 둘 수 없다. 그래서 항목
// (`ConcurrentMapEntry<K,V>`)을 이 컨테이너가 직접 할당/해제한다 -
// `LockFreeQueue::ensureAllocator()`와 동일한 관례로 `AllocFn`/`FreeFn`을
// 주입받는다(이 헤더 자신은 `GenericSlabAllocator`를 몰라도 됨).
//
// **RCU 필요 이유**: `remove()`가 항목을 체인에서 unlink한 뒤에도, 다른
// 코어가 `find()`로 그 항목을 통해 `next`를 따라가는 중일 수 있어
// 즉시 반납하지 않는다 - `Rcu::callAfterGracePeriod()`로 미룬다.
// `find()`의 순회도 `RcuReadGuard`로 감싼다(`LockFreeList`/
// `LockFreeQueue`와 동일한 이유 - 이 코어가 "아직 읽는 중"임을
// grace-period 계산에 알림).
//
// **[정정, 2026-09-17, 착수 전 코드 감사] 원안의 "리사이즈는 전체 락을
// 정해진 순서로 획득 + 재해싱"은 이번 증분에서 제외**(RM-23F4B687
// §4 - 검증 없이 한 번에 다 만들지 않는다) - `BucketCount`를 컴파일
// 타임 템플릿 인자로 고정하는 v1으로 좁혔다. 리사이즈는 "정해진 순서로
// 여러 스트라이프 락을 한꺼번에 잡는" 별도의 데드락 회피 설계가
// 필요해 그 자체로 독립적인 검증 대상이다 - 고정 크기 버킷 배열은
// 이미 존재하는 다른 커널 고정 상한 관례(`kAcpiMaxCpus` 등)와 같은
// 성격의 실용적 절충이며, 필요해지면 별도 후속 계획으로 추가한다.
//
// **[신규] 해시 함수**: 이 커널에 범용 해시 함수가 아직 없어(코드
// 감사로 확인) 이 파일 전용으로 K의 원시 바이트에 대한 FNV-1a를
// 새로 추가했다(`kHashBytes`) - POD 키 타입(정수/포인터 등, 이 커널의
// 실사용 키 후보 전부)에 안전하게 동작하는 표준적인 범용 해시로,
// 새 서브시스템을 설계하는 게 아니라 이 컨테이너가 당장 필요로 하는
// 최소 유틸리티다.
namespace kernel {

template <typename K, typename V>
struct ConcurrentMapEntry {
    AtomicPtr<ConcurrentMapEntry> next;
    K key;
    V value;
    // 이 항목(래퍼) 자신을 나중에 반납할 때 쓸 할당자 - remove()가
    // unlink한 항목을 RCU 유예 콜백(kReclaim, 정적 함수라 ConcurrentMap
    // 인스턴스에 접근할 수 없음)으로 넘길 때, 그 콜백이 어떤 FreeFn을
    // 써야 할지 이 항목 자신에 미리 적어 둔다(LockFreeQueueNode::freeFn
    // 과 동일한 이유).
    void (*freeFn)(void* ptr, uint64_t size) = nullptr;
    RcuCallback rcuCb;
};

template <typename K, typename V, uint32_t BucketCount = 64, uint32_t LockStripes = 16>
class ConcurrentMap {
public:
    using Entry = ConcurrentMapEntry<K, V>;
    using AllocFn = void* (*)(uint64_t size);
    using FreeFn = void (*)(void* ptr, uint64_t size);

    void ensureAllocator(AllocFn allocFn, FreeFn freeFn) {
        if (_alloc != nullptr) {
            return;
        }
        _alloc = allocFn;
        _free = freeFn;
    }

    // 이미 같은 키가 있으면(또는 할당자 미설정/Slab 고갈) false.
    bool insert(const K& key, const V& value) {
        if (!_alloc) {
            return false;
        }
        const uint32_t idx = kBucketIndex(key);
        SpinlockGuard guard(_locks[idx % LockStripes]);
        for (Entry* e = _buckets[idx].load(); e; e = e->next.load()) {
            if (e->key == key) {
                return false;  // 중복 거부(Rbtree/Map과 동일 관례)
            }
        }
        Entry* node = static_cast<Entry*>(_alloc(sizeof(Entry)));
        if (!node) {
            return false;  // 구멍 없음 - 체인은 전혀 안 바뀜
        }
        // [LockFreeQueueNode와 동일한 관례] 원시 할당 메모리라 필드를
        // 전부 명시적으로 채운다 - 실제 placement-new 생성자를 거치지
        // 않는다(Process::allocate()류의 memset+수동 초기화 관례와
        // 같은 이유, RM-23F4B687 §4).
        node->key = key;
        node->value = value;
        node->freeFn = _free;
        node->next.store(_buckets[idx].load());
        _buckets[idx].store(node);  // 이 store 이후에야 다른 코어의
                                     // find()가 이 노드를 볼 수 있다 -
                                     // 그 전까지는 이 스트라이프 락을
                                     // 쥔 이 호출만 아는 사적 메모리.
        return true;
    }

    // 락 없이(lock-free) 순회 - 다른 코어의 insert/remove와 동시에
    // 호출돼도 안전(RCU 유예로 remove()의 unlink된 노드가 아직
    // 살아있음이 보장됨).
    bool find(const K& key, V* outValue) const {
        const uint32_t idx = kBucketIndex(key);
        RcuReadGuard guard;
        for (Entry* e = _buckets[idx].load(); e; e = e->next.load()) {
            if (e->key == key) {
                if (outValue) {
                    *outValue = e->value;
                }
                return true;
            }
        }
        return false;
    }

    // 없으면(또는 할당자 미설정) false.
    bool remove(const K& key) {
        if (!_alloc) {
            return false;
        }
        const uint32_t idx = kBucketIndex(key);
        Entry* removed = nullptr;
        {
            SpinlockGuard guard(_locks[idx % LockStripes]);
            Entry* prev = nullptr;
            Entry* cur = _buckets[idx].load();
            while (cur) {
                if (cur->key == key) {
                    Entry* next = cur->next.load();
                    if (prev) {
                        prev->next.store(next);
                    } else {
                        _buckets[idx].store(next);
                    }
                    removed = cur;
                    break;
                }
                prev = cur;
                cur = cur->next.load();
            }
        }
        if (!removed) {
            return false;
        }
        // unlink 직후에도 다른 코어가 find()로 이 노드를 통해 next를
        // 따라가는 중일 수 있어 즉시 반납하지 않는다 - 유예 기간 이후.
        removed->rcuCb.fn = &kReclaim;
        Rcu::callAfterGracePeriod(&removed->rcuCb);
        return true;
    }

private:
    static uint64_t kHashBytes(const void* data, uint64_t len) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        uint64_t hash = 14695981039346656037ULL;  // FNV-1a offset basis
        for (uint64_t i = 0; i < len; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;  // FNV-1a prime
        }
        return hash;
    }

    static uint32_t kBucketIndex(const K& key) {
        return static_cast<uint32_t>(kHashBytes(&key, sizeof(K)) % BucketCount);
    }

    // RcuCallback -> Entry : offsetof 매크로 대신 멤버 포인터 오프셋
    // (intrusive_list.h/lockfree_queue.h의 kContainerOf/kReclaim과
    // 동일한 관용구 - non-standard-layout 안전).
    static void kReclaim(RcuCallback* cb) {
        const uint64_t offset = reinterpret_cast<uint64_t>(&(reinterpret_cast<Entry*>(0)->rcuCb));
        auto* entry = reinterpret_cast<Entry*>(reinterpret_cast<uint8_t*>(cb) - offset);
        entry->freeFn(entry, sizeof(Entry));
    }

    AllocFn _alloc = nullptr;
    FreeFn _free = nullptr;
    Spinlock _locks[LockStripes];
    AtomicPtr<Entry> _buckets[BucketCount];
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_CONCURRENT_MAP_H
