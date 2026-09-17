#ifndef MINICORE_LIBS_LIBKCONT_LOCKFREE_VECTOR_H
#define MINICORE_LIBS_LIBKCONT_LOCKFREE_VECTOR_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"

// libkcont: LockFreeVector<T>(SP-4DCD0E6A §2, PN-DAE91888) - append-only
// lock-free 동적 배열. 표준 Vector<T,Policy>(vector.h)와 달리 재할당이
// 없다 - 세그먼트 방식(세그먼트 k는 원소 2^k개를 담고, 한 번 확보되면
// 절대 재할당/이동/해제되지 않는다)이라 pushBack 도중에도 이미 확보된
// 원소를 가리키는 포인터가 절대 무효화되지 않는다. 이 구조적 성질
// 덕분에(§2 명시) LockFreeList/LockFreeQueue(PN-013215F9)와 달리 RCU
// 선행 조건이 없다 - 옛 세그먼트를 읽는 리더가 있어도 그 세그먼트
// 자체가 절대 회수되지 않으므로 use-after-free가 구조적으로 불가능.
//
// v1 스코프(§2 명시): pushBack만 지원 - 임의 인덱스 erase/축소는 지원
// 안 함(append-only 전용).
namespace kernel {

template <typename T>
class LockFreeVector {
public:
    using AllocFn = void* (*)(uint64_t size);
    using FreeFn = void (*)(void* ptr, uint64_t size);

    void ensureAllocator(AllocFn allocFn, FreeFn freeFn) {
        if (_alloc == nullptr) {
            _alloc = allocFn;
            _free = freeFn;
        }
    }

    // 실패(세그먼트 할당 고갈) 시 false. 이 호출이 실제로 차지하는
    // 전역 인덱스는 내부적으로 fetchAdd로 예약되며, 그 값 자체는
    // 호출자에게 알려주지 않는다 - "몇 번째로 들어갔는지"는 append-only
    // 컬렉션의 존재 이유(순서 없는 동시 삽입)와 무관하기 때문이다.
    //
    // **알려진 한계(정직하게 인정 - SP-4DCD0E6A §3의 ConcurrentMap/
    // Rbtree 비대칭 인정과 같은 원칙)**: 세그먼트 할당이 실패하면 이미
    // fetchAdd로 예약해버린 전역 인덱스를 안전하게 되돌릴 방법이 없다
    // (그사이 다른 스레드가 이미 다음 인덱스를 예약했을 수 있어, 이
    // 인덱스만 골라 되돌리면 시퀀스에 구멍이 나는 게 아니라 오히려 두
    // 스레드가 같은 자리를 다시 쓰게 된다) - 그래서 할당 실패 시 그
    // 인덱스는 값이 채워지지 않은 채로 영구히 "구멍"으로 남는다
    // (size()는 늘었지만 그 자리를 읽지 않는 건 호출자 책임). 커널
    // 메모리가 실제로 고갈되는 극단적 상황에서만 발생하고, OOM 자체가
    // 이미 비정상 상태라 이 한계를 v1 범위에서 감수한다.
    bool pushBack(const T& value) {
        if (!_alloc) {
            return false;
        }
        const uint64_t index = _size.fetchAdd(1);
        const uint32_t segIndex = kSegmentIndexOf(index);
        T* segment = ensureSegment(segIndex);
        if (!segment) {
            return false;  // 위 "알려진 한계" 참고 - 이 인덱스는 구멍으로 남음
        }
        segment[index - kSegmentBase(segIndex)] = value;
        return true;
    }

    // 호출자가 index < size()를 보장해야 한다 - fetchAdd로 그 인덱스를
    // 예약한 스레드가 pushBack 안에서 이미 ensureSegment를 거쳤으므로,
    // size()가 그 인덱스를 포함한다면 세그먼트도 이미 발행돼 있다
    // (할당 실패로 인한 "구멍"이 아닌 한 - 위 pushBack의 한계 참고).
    T& operator[](uint64_t index) {
        const uint32_t segIndex = kSegmentIndexOf(index);
        T* segment = _segments[segIndex].load();
        return segment[index - kSegmentBase(segIndex)];
    }
    const T& operator[](uint64_t index) const {
        const uint32_t segIndex = kSegmentIndexOf(index);
        T* segment = _segments[segIndex].load();
        return segment[index - kSegmentBase(segIndex)];
    }

    uint64_t size() const { return _size.load(); }
    bool empty() const { return size() == 0; }

private:
    // 세그먼트 k는 원소 2^k개를 담고, 세그먼트 0..k-1의 누적 용량은
    // 2^k - 1개다 - 전역 인덱스 i가 속한 세그먼트는 floor(log2(i+1))
    // (=(i+1)의 최상위 비트 위치), 그 세그먼트 안에서의 오프셋은
    // i - (2^k - 1)이다(Dechev/Pirkelbauer/Stroustrup의 lock-free
    // 동적 배열이 쓰는 표준 세그먼트 분할 공식 - 자체 고안 아님, §2가
    // 요구한 "세그먼트 방식"의 통상적 구현).
    static uint32_t kSegmentIndexOf(uint64_t index) {
        const uint64_t v = index + 1;
        return static_cast<uint32_t>(63 - __builtin_clzll(v));
    }
    static uint64_t kSegmentBase(uint32_t segIndex) {
        return (1ull << segIndex) - 1;
    }
    static uint64_t kSegmentCapacity(uint32_t segIndex) {
        return 1ull << segIndex;
    }

    T* ensureSegment(uint32_t segIndex) {
        T* segment = _segments[segIndex].load();
        if (segment) {
            return segment;
        }
        T* newSegment = static_cast<T*>(_alloc(kSegmentCapacity(segIndex) * sizeof(T)));
        if (!newSegment) {
            return nullptr;
        }
        T* expected = nullptr;
        if (_segments[segIndex].compareExchange(expected, newSegment)) {
            return newSegment;
        }
        // 다른 스레드가 이미 같은 세그먼트를 먼저 발행함 - 이 스레드가
        // 헛되이 만든 세그먼트는 반납하고 이긴 쪽 포인터를 쓴다
        // (compareExchange 실패 시 expected가 현재 값으로 갱신되는
        // libkenv/spinlock.h의 표준 CAS 관례 그대로).
        _free(newSegment, kSegmentCapacity(segIndex) * sizeof(T));
        return expected;
    }

    // 인덱스 최대 2^kMaxSegments-2개까지 - 48이면 2^48개(약 281조)
    // 원소까지 커버해 사실상 고갈될 일이 없다(포인터 배열 자체는
    // 48*8=384바이트로 무시할 크기).
    static constexpr uint32_t kMaxSegments = 48;

    AllocFn _alloc = nullptr;
    FreeFn _free = nullptr;
    AtomicPtr<T> _segments[kMaxSegments];
    AtomicU64 _size{0};
};

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKCONT_LOCKFREE_VECTOR_H
