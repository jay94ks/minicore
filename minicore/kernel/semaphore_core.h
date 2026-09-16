#ifndef MINICORE_KERNEL_SEMAPHORE_CORE_H
#define MINICORE_KERNEL_SEMAPHORE_CORE_H

#include "libkenv/shared_ptr.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "mutex_core.h"  // ParkingPolicy/YieldingPolicy 재사용(§13 - "MutexCore와 거의 동형")
#include "waitable.h"

namespace kernel {

// SP-0666DB3C §13 - Semaphore(§3)/AsyncSemaphore(§10)가 공유하는 상태 +
// 빠른 경로. MutexCore(mutex_core.h)와 완전히 같은 이유로 "확인 실패
// -> onContended 호출"을 한 번의 _guard 보유 구간에서 처리한다
// (잃어버린 웨이크업 방지 - mutex_core.h 상단 주석 참고).
class SemaphoreCore {
public:
    void init(uint32_t initialCount) { _count = initialCount; }

    template <typename OnContended>
    void acquire(OnContended&& onContended) {
        for (;;) {
            _guard.lock();
            if (_count > 0) {
                --_count;
                _guard.unlock();
                return;
            }
            onContended(_guard);
        }
    }

    bool tryAcquire() {
        SpinlockGuard guard(_guard);
        if (_count == 0) return false;
        --_count;
        return true;
    }

    // 카운트++ 후 통지는 Policy 몫(재경쟁 철학, §2.1과 동일).
    template <typename NotifyFn>
    void release(NotifyFn&& notifyOne) {
        SpinlockGuard guard(_guard);
        ++_count;
        notifyOne();
    }

private:
    Spinlock _guard;
    uint32_t _count = 0;
};

// [수정, 2026-09-17, PN-B41D8C0E] mutex_core.h의 BasicMutex와 완전히
// 동일한 이유/계약으로 EnableSharedFromThis를 상속한다 - 자세한 근거는
// BasicMutex 선언부 주석 참고(kMakeShared<Semaphore>()로 만들어야
// Task::blockedOn 강제 cancel()이 성립한다는 제약도 동일).
template <typename Policy>
class BasicSemaphore : public EnableSharedFromThis<BasicSemaphore<Policy>> {
public:
    // [수정, 2026-09-17, PN-B41D8C0E, SP-1DB13F61] mutex_core.h의
    // BasicMutex::BasicMutex()와 동일한 이유(자세한 근거는 그쪽 주석
    // 참고) - `kMakeSharedNew<Semaphore>(initialCount)`로 실제 생성자를
    // 거쳐야 `_policy`의 vtable이 설치된다. `initialCount`는 인스턴스마다
    // 달라 NSDMI로 못 주므로(Mutex와 달리 인자 있는 생성자) 여기서 직접
    // 받는다.
    explicit BasicSemaphore(uint32_t initialCount) { _core.init(initialCount); }

    void acquire() {
        _core.acquire([this](Spinlock& guard) {
            Waitable* w = _policy.waitable();
            WeakPtr<Waitable> self = w ? WeakPtr<Waitable>(this->sharedFromThis(), w) : WeakPtr<Waitable>();
            _policy.onContended(guard, self);
        });
    }
    void release() {
        _core.release([this] { _policy.onRelease(); });
    }
    bool tryAcquire() { return _core.tryAcquire(); }

private:
    SemaphoreCore _core;
    Policy _policy;
};

using Semaphore = BasicSemaphore<ParkingPolicy>;        // §3을 대체(시그니처 동일)
using AsyncSemaphore = BasicSemaphore<YieldingPolicy>;  // §10을 대체(시그니처 동일)

}  // namespace kernel

#endif  // MINICORE_KERNEL_SEMAPHORE_CORE_H
