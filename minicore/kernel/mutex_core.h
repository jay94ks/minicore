#ifndef MINICORE_KERNEL_MUTEX_CORE_H
#define MINICORE_KERNEL_MUTEX_CORE_H

#include "async_task.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "wait_queue.h"

namespace kernel {

// SP-0666DB3C §13 - Mutex(§2)/AsyncMutex(§10)가 공유하는 상태 + 빠른
// 경로. 경합 시 무엇을 하는지는 전혀 모른다(Policy가 결정).
//
// **§13 원문 스케치와의 차이**: 문서의 스케치는 tryAcquire()(락 획득 +
// 해제)와 onContended() 호출을 별개의 두 단계로 그렸는데, 그 사이
// (tryAcquire()가 실패를 반환하고 _guard를 놓은 시점 ~ onContended()가
// 실제로 대기열에 매달리는 시점) 다른 코어의 release()가 끼어들면
// 대기열이 아직 비어 있어 wakeOne()이 헛돌고, 그 뒤 파킹한 대기자는
// 아무도 못 깨우는 잃어버린 웨이크업이 된다(채널 IPC 다중 waiter
// 수정, PN-C9625015에서 실제로 겪은 것과 같은 종류의 경쟁) - 그래서
// 이 구현은 "확인 실패 -> onContended 호출"을 한 번의 _guard 보유
// 구간 안에서 수행한다(Mesa 모니터 패턴, WaitQueue::parkCurrentAndUnlock
// 참고). §13이 확정한 상태/시그니처/재경쟁 철학 자체는 바뀌지 않는다 -
// 순수 구현 세부 수정.
class MutexCore {
public:
    void init() { _locked = false; }

    // 잠금 시도 - 실패하면 _guard를 쥔 채로 onContended(_guard)를
    // 부른다. onContended는 그 안에서 반드시 guard.unlock()을 호출해야
    // 한다(WaitQueue::parkCurrentAndUnlock가 이미 그렇게 하고,
    // YieldingPolicy도 직접 그렇게 한다) - 그래야 다른 코어의
    // unlock()/release()가 계속 진행할 수 있다.
    template <typename OnContended>
    void lock(OnContended&& onContended) {
        for (;;) {
            _guard.lock();
            if (!_locked) {
                _locked = true;
                _guard.unlock();
                return;
            }
            onContended(_guard);
        }
    }

    bool tryAcquire() {
        SpinlockGuard guard(_guard);
        if (_locked) return false;
        _locked = true;
        return true;
    }

    // 다음 소유자를 직접 고르지 않는다(재경쟁 철학, §2.1 그대로) -
    // 상태만 풀고 통지는 Policy 몫.
    template <typename NotifyFn>
    void release(NotifyFn&& notifyOne) {
        SpinlockGuard guard(_guard);
        _locked = false;
        notifyOne();
    }

private:
    Spinlock _guard;
    bool _locked = false;
};

// §1의 WaitQueue를 그대로 재사용하는 정책 - 완전한 kernel::Task를 재운다.
struct ParkingPolicy {
    WaitQueue waitQueue;
    void onContended(Spinlock& guard) { waitQueue.parkCurrentAndUnlock(guard); }
    void onRelease() { waitQueue.wakeOne(); }
};

// §10.2의 yield 반복을 그대로 재사용하는 정책 - 리액터를 블로킹하지 않음.
struct YieldingPolicy {
    void onContended(Spinlock& guard) {
        guard.unlock();
        AsyncTask::yield();
    }
    void onRelease() { /* 대기열 없음 - §10.2 그대로 */ }
};

template <typename Policy>
class BasicMutex {
public:
    void lock() {
        _core.lock([this](Spinlock& guard) { _policy.onContended(guard); });
    }
    void unlock() {
        _core.release([this] { _policy.onRelease(); });
    }
    bool tryLock() { return _core.tryAcquire(); }

private:
    MutexCore _core;
    Policy _policy;
};

using Mutex = BasicMutex<ParkingPolicy>;        // §2를 대체(시그니처 동일)
using AsyncMutex = BasicMutex<YieldingPolicy>;  // §10을 대체(시그니처 동일)

}  // namespace kernel

#endif  // MINICORE_KERNEL_MUTEX_CORE_H
