#ifndef MINICORE_KERNEL_MUTEX_CORE_H
#define MINICORE_KERNEL_MUTEX_CORE_H

#include "async_task.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "scheduler.h"
#include "wait_queue.h"
#include "waitable.h"

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
//
// [수정, 2026-09-17, PN-B41D8C0E, DC-21647E46/QU-4E449C65 답변("(B)
// SharedPtr 별칭 생성자 추가")] `waitable()`은 이 정책이 내부에 실제
// Waitable(WaitQueue)을 갖고 있는지를 BasicMutex/BasicSemaphore가 Policy
// 타입을 몰라도 균일하게 물어볼 수 있게 하는 훅 - YieldingPolicy는
// nullptr을 돌려줘 "이 정책엔 Task::blockedOn으로 노출할 대상이 없다"는
// 뜻을 표현한다(대기열 자체가 없으므로 §9.5 강제 cancel() 대상도 없음).
struct ParkingPolicy {
    WaitQueue waitQueue;
    Waitable* waitable() { return &waitQueue; }
    void onContended(Spinlock& guard, const WeakPtr<Waitable>& self) {
        waitQueue.parkCurrentAndUnlock(guard, self);
    }
    void onRelease() { waitQueue.wakeOne(); }
};

// §10.2의 yield 반복을 그대로 재사용하는 정책 - 리액터를 블로킹하지 않음.
struct YieldingPolicy {
    Waitable* waitable() { return nullptr; }
    void onContended(Spinlock& guard, const WeakPtr<Waitable>& /*self*/) {
        guard.unlock();
        AsyncTask::yield();
    }
    void onRelease() { /* 대기열 없음 - §10.2 그대로 */ }
};

// [수정, 2026-09-17, PN-B41D8C0E] `EnableSharedFromThis<BasicMutex<Policy>>`
// 상속 - ParkingPolicy가 담은 WaitQueue는 자기 컨트롤 블록이 없으므로
// (mutex_core.h 상단 기존 주석 그대로, wait_queue.h 참고), lock()이
// 경합할 때마다 이 Mutex 자신의 컨트롤 블록을 별칭(aliasing)해
// WeakPtr<Waitable>을 만들어 Task::blockedOn에 심는다. **이 메커니즘이
// 성립하려면 이 Mutex가 반드시 kMakeShared<Mutex>()로 만들어져 있어야
// 한다** - 스택/정적 인스턴스는 `_weakThis`가 비어 있어 sharedFromThis()
// 가 항상 빈 SharedPtr을 반환하고, 그 결과 Task::blockedOn도 항상 빈
// WeakPtr로 남는다(task.h의 blockedOn 주석 참고 - 파킹/wakeOne 자체는
// 여전히 정상 동작하지만 §9.5 강제 cancel()만 조용히 무력화됨). 이
// 컨트롤 블록 접근(sharedFromThis() 안의 CAS)은 경합(contention) 발생
// 시에만 타는 느린 경로라 tryLock()/무경합 lock() 빠른 경로엔 원자
// 연산이 추가되지 않는다.
template <typename Policy>
class BasicMutex : public EnableSharedFromThis<BasicMutex<Policy>> {
public:
    // [수정, 2026-09-17, PN-B41D8C0E, SP-1DB13F61] 실제 생성자 -
    // `kMakeSharedNew<Mutex>()`(placement new)로 만들어야 `_policy`
    // (WaitQueue 서브오브젝트 포함)의 vtable이 실제로 설치된다(§2 -
    // "Mutex 생성자가 실행되면 그 안의 WaitQueue 서브오브젝트 생성자도
    // C++ 언어 규칙상 자동으로 함께 실행된다"). `_policy`는 멤버
    // 초기화 목록에 안 올려도 암시적 기본 생성자가 그대로 실행되므로
    // (NSDMI뿐이라 인자 필요 없음) 이 생성자 본문은 `_core`의 실행
    // 시점 상태(`_locked`)만 채우면 된다.
    BasicMutex() { _core.init(); }

    void lock() {
        _core.lock([this](Spinlock& guard) {
            Waitable* w = _policy.waitable();
            WeakPtr<Waitable> self = w ? WeakPtr<Waitable>(this->sharedFromThis(), w) : WeakPtr<Waitable>();
            _policy.onContended(guard, self);
        });
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

// [신규, 2026-09-26, PN-D168A778 curspace 실시간 갱신] libkenv/spinlock.h의
// SpinlockGuard와 같은 RAII 관례를 Mutex/AsyncMutex/ReentrantMutex/
// ReentrantAsyncMutex(전부 인자 없는 lock()/unlock() 시그니처를 공유)에도
// 그대로 적용한 얇은 템플릿 래퍼 - 코루틴 안에서 여러 개의 조기 `break`/
// `return`으로 빠져나가는 임계구역을 다룰 때 매 탈출 경로마다 수동
// unlock()을 빼먹을 위험을 없앤다(스택 언와인딩만으로 소멸자가 불리므로
// C++ 예외를 안 쓰는 이 커널에서도 그대로 성립).
template <typename Lockable>
class LockGuard {
public:
    explicit LockGuard(Lockable& lockable) : _lockable(lockable) { _lockable.lock(); }
    ~LockGuard() { _lockable.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Lockable& _lockable;
};

// [신규, 2026-09-22, PN-4D60D49C, SP-0666DB3C §16 확정 설계] 재진입
// Mutex/AsyncMutex - §13의 Mutex/AsyncMutex(위)와 완전히 별개인 타입
// (컴파일 타임에 재진입 지원 여부 강제, §16.1). 항상 소유자를
// 추적한다 - "소유자 없음" 모드 자체가 없다(그건 위 MutexCore/
// BasicMutex의 몫).
class ReentrantMutexCore {
public:
    void init() {
        _locked = false;
        _owner = nullptr;
        _recursionDepth = 0;
    }

    // MutexCore::lock()과 동일한 콜백 루프 구조 - _guard를 쥔 채로
    // onContended를 부르고, onContended가 그 안에서 guard.unlock()까지
    // 책임진다. owner가 이미 이 락을 쥔 소유자와 같으면(포인터 동일성
    // 비교) 대기 없이 재귀 깊이만 늘리고 즉시 반환한다.
    template <typename OnContended>
    void lock(const void* owner, OnContended&& onContended) {
        for (;;) {
            _guard.lock();
            if (!_locked) {
                _locked = true;
                _owner = owner;
                _recursionDepth = 1;
                _guard.unlock();
                return;
            }
            if (owner == _owner) {
                ++_recursionDepth;
                _guard.unlock();
                return;
            }
            onContended(_guard);
        }
    }

    // 재귀 깊이가 0으로 떨어질 때만 실제로 풀고 통지한다 - 그 전까지는
    // 같은 소유자의 중첩 unlock()일 뿐이라 아무도 깨울 필요가 없다.
    template <typename NotifyFn>
    void release(NotifyFn&& notifyOne) {
        SpinlockGuard guard(_guard);
        if (--_recursionDepth > 0) {
            return;
        }
        _locked = false;
        _owner = nullptr;
        notifyOne();
    }

private:
    Spinlock _guard;
    bool _locked = false;
    const void* _owner = nullptr;
    uint32_t _recursionDepth = 0;
};

// 동기 재진입 뮤텍스 - 소유자는 `Scheduler::currentTask()`(항상 어디서든
// 조회 가능)로 식별한다. **[정정, 2026-09-22, PN-4D60D49C 구현 세션]**
// SP-0666DB3C §16.1 원 스케치는 `_waitQueue.parkCurrentAndUnlock(guard)`를
// 인자 1개로 불렀으나, 실제 `WaitQueue::parkCurrentAndUnlock()`(wait_queue.h)
// 시그니처는 `(Spinlock&, const WeakPtr<Waitable>&)` 2개를 요구한다 -
// 위 `Mutex`(`BasicMutex<ParkingPolicy>`)와 동일한 이유(§9.5 강제
// cancel()이 `Task::blockedOn`으로 이 WaitQueue를 찾으려면 그 자신의
// 컨트롤 블록을 별칭한 `WeakPtr<Waitable>`이 필요)로 `EnableSharedFromThis`
// 상속 + 컨트롤 블록 별칭 등록을 그대로 이식했다 - 재진입 판정 로직
// 자체(위 `ReentrantMutexCore`)는 원 설계 그대로, 순수하게 실제
// `WaitQueue` API와 맞물리도록 형태만 보강. `Mutex`와 동일하게 반드시
// `kMakeSharedNew<ReentrantMutex>()`로 만들어야 한다(그래야 `_weakThis`가
// 채워져 `sharedFromThis()`가 유효한 값을 반환).
class ReentrantMutex : public EnableSharedFromThis<ReentrantMutex> {
public:
    ReentrantMutex() { _core.init(); }

    void lock() {
        const void* owner = static_cast<const void*>(Scheduler::currentTask());
        _core.lock(owner, [this](Spinlock& guard) {
            WeakPtr<Waitable> self(this->sharedFromThis(), static_cast<Waitable*>(&_waitQueue));
            _waitQueue.parkCurrentAndUnlock(guard, self);
        });
    }
    void unlock() {
        _core.release([this] { _waitQueue.wakeOne(); });
    }

private:
    ReentrantMutexCore _core;
    WaitQueue _waitQueue;
};

// 비동기 재진입 뮤텍스 - 소유자는 `AsyncTask::current()`(SP-0666DB3C
// §15.2/async_task.h, 이 세션이 기존 `gCurrentAsyncTask[coreIndex]`
// 위에 새로 노출함)로 조회한다. `YieldingPolicy`와 마찬가지로 대기열이
// 없어(경합 시 바로 yield 후 재시도) `EnableSharedFromThis`/컨트롤
// 블록 별칭이 필요 없다 - lock()/unlock() 시그니처도 파라미터 없이
// 유지된다(§16.1 "OwnerId 파라미터 방식은 AsyncTask::current()로
// 완전히 대체돼 더 이상 필요 없다" 결론 그대로).
class ReentrantAsyncMutex {
public:
    void lock() {
        const void* owner = static_cast<const void*>(AsyncTask::current());
        _core.lock(owner, [](Spinlock& guard) {
            guard.unlock();
            AsyncTask::yield();
        });
    }
    void unlock() {
        _core.release([] {});
    }

private:
    ReentrantMutexCore _core;
};

// [SP-0666DB3C §16.2] Semaphore는 재진입 대상에서 제외 - 카운팅
// 프리미티브라 "소유자가 자기 자신을 다시 획득"이라는 재진입 개념
// 자체가 성립하지 않는다(설계자 승인, QU-C06793C2). ReentrantSemaphore
// 류는 만들지 않는다.

// [신규, 2026-09-26, SP-33FE698A, DC-59F63D0E/QU-5CE6FA42 설계자
// 지시("이걸 설계안으로 작성해봐")] 진짜 C++20 코루틴(`co_return`이
// 있어 컴파일러가 상태 기계로 변환한 함수, 예: `Ext4Driver::onExec`)
// 안에서 경합 시 폴링/재시도 없이 진짜로 정지했다가 `release()`가
// 명시적으로 깨워 주는 뮤텍스 - 위 `Mutex`(`ParkingPolicy`, 진짜
// `kernel::Task`를 재움)와 `AsyncMutex`(`YieldingPolicy`, `AsyncTask::
// yield()` - 스택풀 전용, 코루틴에서 부르면 무한 대기)를 코루틴에
// 못 쓴다는 공백을 메운다. `BasicMutex<Policy>::lock()`은 평범한
// 비-코루틴 함수라 그 자리에 새 Policy를 끼우는 방식으로는 코루틴을
// 정지시킬 수 없다(정지 지점이 호출부 코루틴 자신의 `co_await` 식
// 이어야 한다는 C++20 언어 규칙) - 그래서 `BasicMutex<Policy>`를
// 확장하지 않고 `AsyncTaskCoroAwaiter`/`AsyncTaskCoroYield`와 같은
// 급의 독립된 co_await 가능 프리미티브로 새로 만든다.
//
// `interrupt_subscription.cpp`의 `WaitInterruptHandler`가 이미
// 실사용 중인 패턴을 이름만 일반화했다 - 대기자는 `kernel::
// SuspendAlways{}`로 순수 정지(스스로 재큐잉하지 않으므로 "재시도"
// 개념 자체가 없음), 깨우기는 소유자가 대기열에서 직접 `popFront()`
// 한 뒤 `AsyncReactor::submitCompletion()`을 호출(`kInterruptSubscriptionIsr`
// 의 `popFront()`+`submitCompletion()`과 동일). 대기열 노드는
// `AsyncTask::next`를 재사용하는 침습적 FIFO(`channel.h`의
// `AsyncTaskWaitQueue`/`interrupt_subscription.h`의
// `InterruptWaiterQueue`와 완전히 같은 구조 - 이 작은 구조체를
// 서브시스템마다 독립적으로 다시 두는 게 이 커널의 의도된 관례,
// "관계없는 두 서브시스템의 불필요한 결합 방지").
//
// **"직접 인계"(direct handoff)**: `release()`는 대기열이 비어
// 있지 않으면 `_locked`를 여전히 true로 유지한 채 다음 대기자에게
// 소유권을 그대로 넘긴다(그 대기자의 `await_resume()`이 "이미 락을
// 쥔 채로 재개됐다"는 뜻) - 기존 `WaitQueue::wakeOne()`/
// `ParkingPolicy`의 "깨우고 재경쟁"과 달리, `_locked=false`가 되는
// 순간 자체가 "대기자가 없을 때"로 한정되므로 release와 동시에
// 다른 코어의 새 `tryAcquire()`가 새치기할 여지가 없다 - 순서가
// 항상 등록 순서 그대로(FIFO) 확정되고, "재시도 카운터"/"드레인
// 배치 상한" 같은 스케줄러 공정성 파라미터에 전혀 의존하지 않는다
// (`PN-F2594E93`의 "(b)" 수정이 실제 AHCI I/O처럼 임계구역이 여러
// `co_await`를 연달아 감싸는 경우 여전히 hang했던 근본 원인 - 재시도
// 폴링 자체를 없애 그 원인이 무엇이든 성립할 수 없게 만든다).
//
// **취소(cancel) 처리 - 이 클래스를 쓰는 각 AsyncTaskHandler의
// 책임**: `WaitInterruptHandler::onCancel`이 이미 겪은 것과 같은
// 위험이 그대로 적용된다 - 대기열에 매달린 채로 그 AsyncTask의
// 소유자(제출자 프로세스)가 강제 종료되면, 프레임워크가 onExec을
// 재개하는 대신 onCancel만 부르고 코루틴 프레임 자체를 반납한다 -
// 그 시점에 `_waiters`에 남아 있는 포인터가 댕글링돼 다음
// `release()`의 `popFront()`가 UAF를 일으킨다(`SP-1FBC0EEB` Channel
// IPC의 `PN-C4611402`, interrupt_subscription.cpp의 동일 클래스
// 결함과 같은 패턴) - 이 뮤텍스를 쓰는 각 `AsyncTaskHandler`의
// `onCancel`이 "이 task가 대기열에 매달려 있으면 제거한다"를
// 반드시 구현해야 한다(`removeIfWaiting()` 참고).
class AsyncCoroMutex {
public:
    void init() {
        _locked = false;
        _waiters.head = nullptr;
        _waiters.tail = nullptr;
    }

    // co_await 가능한 획득 - 진짜 C++20 코루틴 onExec 안에서만 쓴다
    // (AsyncTaskCoroAwaiter/AsyncTaskCoroYield와 동일한 제약 - 코루틴
    // 밖에서 부르면 AsyncTask::current()가 nullptr이라 안전하지
    // 않다).
    class LockAwaiter {
    public:
        explicit LockAwaiter(AsyncCoroMutex& mutex) : _mutex(mutex) {}
        // 항상 await_suspend 안에서 "확인+등록"을 한 스핀락 보유
        // 구간으로 원자적으로 처리해야 lost-wakeup을 피할 수 있다
        // (MutexCore::tryAcquireOrKeepLock/WaitInterruptHandler §3
        // "onExec 원자성 계약"과 동일한 원칙) - 그래서 await_ready()는
        // 항상 false만 반환해 반드시 await_suspend를 거치게 한다.
        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<>) noexcept {
            AsyncTask* self = AsyncTask::current();
            SpinlockGuard guard(_mutex._lock);
            if (!_mutex._locked) {
                _mutex._locked = true;
                return false;  // 무경합 - 정지 없이 즉시 재개(락 획득 완료)
            }
            _mutex._waiters.pushBack(self);
            return true;  // 정지 - release()의 직접 인계가 재개시킴(그때 이미 락 보유 상태)
        }
        void await_resume() noexcept {}

    private:
        AsyncCoroMutex& _mutex;
    };
    LockAwaiter lockAsync() { return LockAwaiter(*this); }

    // 즉시 시도 - 기존 MutexCore::tryAcquire()와 동일한 의미(비-코루틴
    // 호출부/점진 이행 호환용).
    bool tryAcquire() {
        SpinlockGuard guard(_lock);
        if (_locked) return false;
        _locked = true;
        return true;
    }

    // [PN-6D2C8836, SP-33FE698A §2.4 실측 확정] onCancel의 removeIfWaiting
    // 만으로는 부족하다 - 실제로 재현됨: 대기자가 취소되면
    // `Scheduler::cancelPendingSyscalls()`가 `state=Cancelled`로 바꾸고
    // 그 즉시 `submitCompletion()`으로 큐에 다시 넣지만(onCancel은 그
    // 큐 항목이 나중에 drainOnce()에 뽑힐 때에야 비로소 불린다), 그
    // "나중"이 오기 전에 락 보유자가 `release()`를 부르면 `_waiters`
    // 에는 아직 그 대기자가 남아 있어 여기서 또 `submitCompletion()`을
    // 부르게 된다 - 같은 AsyncTask가 두 큐(또는 같은 큐에 두 번) 노드로
    // 동시에 걸려 침습적 리스트가 깨진다. 그래서 여기서 직접 인계
    // 대상을 고를 때 이미 취소된 항목은 건너뛴다(그 항목은 자신의
    // 기존 큐 등록만으로 나중에 onCancel이 알아서 청소한다) - 이렇게
    // 하면 release() 쪽에서 다시 큐에 넣는 일 자체가 없어져 이 경쟁이
    // 사라진다. **잔여 위험**: 이 상태 읽기 자체는 락 없이 이뤄져(다른
    // 필드 접근과 동일한 이 코드베이스 기존 관례) `state` 대입과의
    // 진짜 원자성은 없다 - "그 즉시" 취소되는 아주 좁은 타이밍은 여전히
    // 이론상 남는다(DC 등록 예정).
    void release() {
        AsyncTask* next = nullptr;
        for (;;) {
            {
                SpinlockGuard guard(_lock);
                next = _waiters.popFront();
                if (!next) {
                    _locked = false;
                    return;
                }
                // next가 있으면 _locked=true 유지 - 위 "직접 인계" 참고.
            }
            if (next->state == AsyncTaskState::Cancelled) {
                continue;  // 이미 취소됨 - 건너뛰고 다음 대기자를 본다.
            }
            break;
        }
        AsyncReactor::submitCompletion(next, /*preemptive=*/true);
    }

    // 취소 경로 전용 - 이 task가 아직 대기열에 매달려 있으면(아직
    // release()가 꺼내 깨우기 전) 제거한다. 이미 꺼내져 재개
    // 대기 중이거나 이미 락을 보유 중이면 false(할 일 없음) - 호출부
    // (각 AsyncTaskHandler::onCancel)가 이 반환값으로 추가 정리가
    // 필요한지 판단할 수 있다. 선형 탐색(WaitInterruptHandler와 동일
    // 관례 - 대기자 수가 많지 않다는 전제, kMaxSubscribersPerVector류
    // 상한과 같은 급).
    bool removeIfWaiting(AsyncTask* task) {
        SpinlockGuard guard(_lock);
        return _waiters.remove(task);
    }

private:
    // interrupt_subscription.h의 InterruptWaiterQueue/channel.h의
    // AsyncTaskWaitQueue와 동일한 구조 - 이 파일 안에 독립적으로
    // 다시 둔다(관계없는 서브시스템 간 불필요한 결합 방지 관례).
    struct Waiters {
        AsyncTask* head = nullptr;
        AsyncTask* tail = nullptr;

        void pushBack(AsyncTask* task) {
            task->next.store(nullptr);
            if (tail) {
                tail->next.store(task);
            } else {
                head = task;
            }
            tail = task;
        }

        AsyncTask* popFront() {
            AsyncTask* task = head;
            if (task) {
                head = task->next.load();
                if (!head) {
                    tail = nullptr;
                }
                task->next.store(nullptr);
            }
            return task;
        }

        // removeIfWaiting() 전용 - 침습적 단일 연결 리스트에서 임의
        // 원소 하나를 제거(선형 탐색, 이전 노드를 추적하며 진행).
        bool remove(AsyncTask* target) {
            AsyncTask* prev = nullptr;
            AsyncTask* cur = head;
            while (cur) {
                AsyncTask* nextNode = cur->next.load();
                if (cur == target) {
                    if (prev) {
                        prev->next.store(nextNode);
                    } else {
                        head = nextNode;
                    }
                    if (cur == tail) {
                        tail = prev;
                    }
                    cur->next.store(nullptr);
                    return true;
                }
                prev = cur;
                cur = nextNode;
            }
            return false;
        }
    };

    Spinlock _lock;
    bool _locked = false;
    Waiters _waiters;
};

// SpinlockGuard/LockGuard<T>와 같은 RAII 관례 - AsyncCoroMutex는
// tryAcquire()/release() 짝(인자 없는 lock()/unlock()이 아님)이라
// 위 LockGuard<T> 템플릿 대상이 아니다(MutexCore와 동일한 이유).
// co_await로 이미 획득이 끝난 뒤(LockAwaiter가 반환된 뒤) 생성해
// 스코프 종료 시 항상 release()를 호출한다.
class AsyncCoroMutexReleaseGuard {
public:
    explicit AsyncCoroMutexReleaseGuard(AsyncCoroMutex& mutex) : _mutex(mutex) {}
    ~AsyncCoroMutexReleaseGuard() { _mutex.release(); }
    AsyncCoroMutexReleaseGuard(const AsyncCoroMutexReleaseGuard&) = delete;
    AsyncCoroMutexReleaseGuard& operator=(const AsyncCoroMutexReleaseGuard&) = delete;

private:
    AsyncCoroMutex& _mutex;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_MUTEX_CORE_H
