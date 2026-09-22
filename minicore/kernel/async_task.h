#ifndef MINICORE_KERNEL_ASYNC_TASK_H
#define MINICORE_KERNEL_ASYNC_TASK_H

#include "interrupt_frame.h"
#include "libkenv/chunked_list.h"
#include "libkenv/coroutine.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"
#include "task.h"  // TaskOwnerRef(submitterTask, PN-C536F352) - 완전한 정의 필요
#include "waitable.h"

namespace kernel {

// 커널 전용 비동기 프레임워크(SP-F682B889, 확정) - kernel::Task보다
// 훨씬 가벼운 전용 스택을 쓰는 스케줄링 가능 단위. Task와 같은
// 소프트웨어 컨텍스트 전환(kContextSwitch)을 그대로 재사용한다(구조가
// 거의 동일 - tcb 오프셋 0 고정, task.h의 Task와 같은 불변조건).
using AsyncTaskSubjectCode = uint32_t;   // 어느 AsyncTaskHandler에 속하는지
using AsyncTaskManageCode = uint64_t;    // 그 작업 주체 안에서 이 인스턴스를 식별하는 관리 코드

// Cancelled(PN-40E976F2) - 이 AsyncTask를 기다리던 UserThread가
// 완료/실패보다 먼저 죽어, 결과를 가져갈 사람이 아무도 남지 않았을 때
// 전이하는 상태. onExec을 실행/재개하지 않고 바로 AsyncTaskHandler::
// onCancel만 호출한 뒤 프레임워크가 자원을 반납한다(AsyncReactor::
// drainOnce, scheduler.cpp의 SelfTerminateHandler::onExec 참고).
enum class AsyncTaskState { Ready, Running, Suspended, Completed, Failed, Cancelled };

// 전용 스택 크기 - PL-1E247831이 "정확한 크기는 실측 확정"으로 남겨둔
// 값(v1 시작값 4KiB, GenericSlabAllocator::alloc이 2048B 초과 요청을
// PageFrameAllocator 직행 경로(Order 0)로 자동 위임하므로 그대로 재사용
// 가능하다(페이지 하나를 그대로 받는다).
constexpr uint64_t kAsyncTaskStackSize = 4096;

class AsyncToken;  // 전방 선언 - AsyncTokenSource::token()이 필요로 함

// SP-F682B889 §8(설계자 지시, 2026-09-14) - "AsyncTask에 AsyncToken,
// AsyncTokenSource를 도입하고, 이게 트리거되면 취소된 걸로 간주하는
// 메커니즘". freestanding 환경에는 임의 지점에서 스택을 안전하게
// 되감는 장치가 없어(C++ 예외 자체를 안 씀, RM-23F4B687) 강제 중단이
// 아니라 협조적(cooperative) 신호로 푼다 - 실행 중인 코드 스스로
// "지금 취소됐는지"를 확인하고 자기 판단으로 조기 종료한다(§8.4).
// AsyncTask 하나당 정확히 하나씩 값으로 소유된다(힙 할당/참조 카운트
// 없음, 아래 AsyncTask::cancelSource 필드) - 한 번 트리거되면
// 되돌릴 수 없다(단방향).
class AsyncTokenSource {
public:
    // 취소를 요청한다 - 멱등(이미 트리거된 상태에서 다시 불러도
    // 안전, 아무 일도 하지 않는다). 어느 코어에서 불러도 안전
    // (AtomicU32 사용, 별도 Spinlock 불필요).
    void trigger() { _triggered.store(1); }

    bool isTriggered() const { return _triggered.load() != 0; }

    // 이 소스를 가리키는 경량 읽기 전용 핸들을 발급한다 - 값 복사
    // 가능, 소유권 없음(AsyncTokenSource보다 오래 살아있으면 안 됨 -
    // 항상 그 소스를 담은 AsyncTask보다 짧게 유지).
    AsyncToken token() const;

private:
    AtomicU32 _triggered;
};

// onExec/코루틴 본문이 폴링하는 뷰 - AsyncTokenSource*를 감싼 얇은
// 값 타입.
class AsyncToken {
public:
    explicit AsyncToken(const AsyncTokenSource* source) : _source(source) {}

    bool isCancelled() const { return _source != nullptr && _source->isTriggered(); }

private:
    const AsyncTokenSource* _source;
};

inline AsyncToken AsyncTokenSource::token() const {
    return AsyncToken(this);
}

// [PN-C62F7908, SP-F682B889 §7.3] AsyncTaskHandler::onExec의 코루틴
// 반환 타입 - 설계자 답변("C++ 코루틴은 가상함수를 지원... 가상함수
// 정의를 코루틴으로 바꿔")대로 onExec 자신의 시그니처 자체를 이
// 타입으로 바꾼다(코루틴 전용 별도 가상 메서드를 새로 두지 않음).
// `co_await`를 전혀 안 쓰는 본문도 이 반환 타입으로 그냥 컴파일되고
// (C++20 코루틴 변환은 본문의 `co_*` 사용 여부만으로 결정, 반환
// 타입 자체와는 무관) 첫 실행에서 곧바로 완료 상태로 끝난다 -
// "코루틴 대신 평범한 함수" 취지는 그대로 유지된다.
//
// **initial_suspend=SuspendNever**: onExec 호출 즉시 본문 실행을
// 시작한다(호출부가 이 반환값을 받기 전에 첫 co_await 지점까지, 또는
// 끝까지 동기적으로 실행됨) - kAsyncTaskEntryWrapper가 "onExec을
// 호출하면 그 자리에서 실행이 시작된다"고 기대하는 기존 관례와 일치.
//
// **final_suspend=SuspendAlways**: 완료 직후 자동으로 프레임을 정리
// (destroy)하지 않고 그대로 suspend 상태로 남긴다 - 호출부
// (kAsyncTaskEntryWrapper/AsyncReactor::drainOnce())가 `done()`으로
// "이번에 완료됐는지"를 확인한 뒤 필요한 정리(예: coroHandle을 비움)
// 를 할 기회를 갖기 위함이다. 코루틴 프레임 자체의 실제 해제는
// AsyncTask 프레임워크가 그 AsyncTask 구조체/스택을 반납하는 시점에
// 맞춰 명시적으로 처리한다(§3.1 "생성/해제 전부 처리기 책임"과 같은
// 정신 - 프레임워크가 자동으로 뒤에서 해제하지 않음).
class AsyncExecCoro {
public:
    struct promise_type {
        AsyncExecCoro get_return_object() {
            return AsyncExecCoro{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        SuspendNever initial_suspend() noexcept { return {}; }
        SuspendAlways final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        // freestanding - C++ 예외를 안 쓴다(RM-23F4B687). 컴파일러
        // 프로토콜상 이 메서드 자체는 있어야 하지만 호출될 일이 없다.
        void unhandled_exception() noexcept {}

        // noexcept 필수(C++20 표준 - get_return_object_on_allocation_failure()
        // 를 선언한 promise_type의 operator new는 반드시 noexcept여야
        // 컴파일러가 예외 대신 nullptr 반환/실패 콜백 경로를 쓴다).
        // -fno-exceptions 빌드라 애초에 예외를 던질 수도 없다.
        static void* operator new(size_t size) noexcept { return GenericSlabAllocator::alloc(size); }
        static void operator delete(void* ptr, size_t size) { GenericSlabAllocator::free(ptr, size); }

        // [PN-C62F7908 5/5, §7.2 확정, QU-5E58E361 설계자 답변 -
        // "get_return_object_on_allocation_failure로 해"] operator new가
        // (Slab 고갈로) nullptr을 반환하면 코루틴 본문은 전혀 실행되지
        // 않고(실측 확인, §7.2) 이 정적 메서드의 반환값이 그대로
        // handler->onExec(...)의 결과가 된다 - 빈 핸들을 담은
        // AsyncExecCoro를 반환해 "실패"를 표현한다. 호출부
        // (kAsyncTaskEntryWrapper)는 이걸 AsyncTaskState::Failed가
        // 아니라 **재시도 트리거**로 해석해야 한다(§7.2 - 실패로
        // 취급하면 이미 확정된 "yield하며 재시도" 결정과 충돌한다).
        static AsyncExecCoro get_return_object_on_allocation_failure() noexcept { return AsyncExecCoro{}; }
    };

    AsyncExecCoro() = default;
    explicit AsyncExecCoro(std::coroutine_handle<promise_type> handle) : _handle(handle) {}

    // 지금 이 시점에 완료됐는지(co_await 없이 끝까지 실행됐거나,
    // 이전에 suspend된 코루틴이 이번 resume()으로 끝까지 도달함).
    bool done() const { return !_handle || _handle.done(); }

    std::coroutine_handle<> handle() const { return _handle; }

    // final_suspend=SuspendAlways라 자동 정리되지 않으므로, 호출부가
    // done()==true를 확인한 뒤 직접 불러야 한다 - 두 번 부르면
    // 안 된다(호출부가 coroHandle을 비워 재호출을 막을 책임을 진다).
    void destroy() {
        if (_handle) {
            _handle.destroy();
            _handle = nullptr;
        }
    }

private:
    std::coroutine_handle<promise_type> _handle;
};

struct AsyncTask;  // 포인터로만 참조(AsyncTaskWeakRef 아래) - 전체 정의는 바로 다음

// [PN-D01B7D07, SP-F682B889 §3.7, 설계자 답변 - "lock-free 기반으로
// 약한 참조를 구현해"] AsyncTask 타임아웃(§3.7)이 겪는 use-after-free
// 문제(타이머가 울리기 전에 AsyncTask가 이미 반납/재사용될 수 있음)를
// AsyncTask 구조체 자체에 새 필드를 잔뜩 추가하거나 모든 반납 지점에
// 개별 배선을 하는 대신, 독립적으로 힙 할당되는 이 작은 컨트롤
// 블록으로 해결한다 - AsyncTask 쪽은 포인터 하나(`weakRef`)만 갖고,
// 실제 무효화/수명 판단은 전부 이 클래스 안에서 원자적으로 끝난다.
//
// [갱신, 2026-09-18, SP-76250478 §3.1, PN-0EB2FABF] 원래 async_task.cpp
// 안에만 있던 완전 비공개 구현이었으나(주석 원문: "구현 세부는 이
// 헤더의 소비자가 알 필요 없음 - AsyncTask::scheduleTimeout()만 공개
// API"), `Join` syscall(process.cpp)이 **똑같은 문제**(정지된 Join
// AsyncTask를 다른 핸들러(`SelfTerminateThreadHandler`, scheduler.cpp)
// 가 안전하게 참조/재개해야 함 - 대상 UserThread가 그사이 먼저 죽어도
// UAF 없이)를 겪어, 이 클래스를 async_task.cpp 전용에서 헤더의 진짜
// 공개 재사용 primitive로 승격했다(순수 이동 - 로직 변화 없음). 참조
// 카운트 시작값만 일반화했다: 예전엔 "AsyncTask 쪽 몫 1 + 타이머 쪽
// 몫 1 = 2"로 고정이었으나, 이제 관찰자가 몇 명이든(타임아웃/Join
// 둘 다 동시에 걸 수도 있음) `init()`이 "AsyncTask 자신의 몫"인 1로만
// 시작하고, 새 관찰자가 생길 때마다 `addRef()`를 명시적으로 한 번씩
// 부른다(`scheduleTimeout()`이 이미 그렇게 갱신됨) - `kReleaseAsyncTask()`
// 가 여전히 "AsyncTask 쪽 몫" 하나만 `release()`하는 기존 동작은
// 그대로라 이 일반화가 기존 타임아웃 동작을 바꾸지 않는다.
//
// - `lock()` - 아직 무효화되지 않았으면 그 순간의 `AsyncTask*`를
//   반환(그 뒤로도 안전하게 역참조할 수 있다 - 왜 안전한지는
//   `invalidate()`가 반드시 `kReleaseAsyncTask()` "안에서" 실제 반납보다
//   **먼저** 불린다는 보장 덕분이다: `lock()`이 non-null을 반환했다는
//   건 그 반환 시점에 아직 무효화 전이었다는 뜻이고, 같은 코어 위에서
//   순차 실행되는 이 커널에 진짜 동시 실행 경쟁은 없다).
// - `invalidate()` - AsyncTask가 실제로 반납되는 바로 그 순간
//   (`kReleaseAsyncTask()`)에만 부른다 - 이후 `lock()`은 항상 nullptr.
// - `addRef()`/`release()` - "이 컨트롤 블록 자체"의 참조 카운트를
//   늘리고/줄인다, 0이 되면(마지막 참여자) 블록 자신을 반납한다.
//   각 관찰자(AsyncTask 자신 포함)는 자기 볼일이 끝나면(AsyncTask는
//   반납 시, 타임아웃은 발화 시, Join은 재개 처리 시) 정확히 자신이
//   `addRef()`(또는 `init()`의 암묵적 +1)한 만큼만 `release()`를
//   불러야 한다.
class AsyncTaskWeakRef {
public:
    // 이 프로젝트 전역 관례대로 placement new를 쓰지 않는다(raw slab
    // 메모리 위에 reinterpret_cast로 앉힌 뒤 명시적으로 초기화 -
    // AsyncTask::init()/chunked_list.h와 동일한 패턴). AsyncTask 자신의
    // 몫(1)으로만 시작한다 - 추가 관찰자는 각자 addRef()를 부른다.
    void init(AsyncTask* target) {
        _target.store(target);
        _refCount.store(1);
    }

    AsyncTask* lock() const { return _target.load(); }
    void invalidate() { _target.store(nullptr); }
    void addRef() { _refCount.fetchAdd(1); }

    void release() {
        if (_refCount.fetchSub(1) == 1) {
            GenericSlabAllocator::free(this, sizeof(AsyncTaskWeakRef));
        }
    }

private:
    AtomicPtr<AsyncTask> _target;
    AtomicU32 _refCount;
};

// [신규, 2026-09-19, PN-0AC554C2/PN-EA968DF0, QU-25E1C297 답변 +
// docs opinion + QU-B89531F0 답변("제안대로 진행")] AsyncTask 본체는
// 기존 비동기 프레임워크(AsyncTask::submit 등)에 그대로 제출하고,
// 이 얇은 래퍼만 `Task::blockedOn` 리스트에 들어간다 - "Task가
// 대기해야 하는 모든 것을 Waitable로 wrapping"하라는 답변을 AsyncTask
// 자신을 고치지 않고 만족시키는 자리(AsyncTask는 SharedPtr로 관리되는
// 타입이 아니라 WeakPtr<Waitable>이 바로 아일리어싱할 SharedPtr
// 컨트롤 블록이 없다 - 그래서 별도 래퍼가 필요했다).
//
// - `isCompleted()` - 대상 AsyncTask가 Completed/Failed/Cancelled에
//   도달했는지. 대상이 이미 반납돼 `AsyncTaskWeakRef::lock()`이
//   nullptr을 돌려주면(정상적으로 다 처리되고 사라진 경우) 완료로
//   간주한다 - 영원히 리스트에 남아 있으면 안 되므로.
// - `cancel()` - §8 협조적 취소(QU-25E1C297 답변: "각 비동기 작업은
//   취소 토큰을 내부적으로 유통하여 취소되었으면 탈출하는 메커니즘이
//   필수") - `AsyncTask::cancelSource`가 이미 정확히 이 트리거 지점을
//   예고해 뒀다(주석 "SP-0666DB3C §9 Waitable::cancel() 경로(아직
//   미연동)") - 강제로 죽이지 않고 토큰만 세운다, 실제 탈출은 그
//   AsyncTaskHandler::onExec() 자신이 `cancelSource.token().
//   isCancelled()`를 확인해 스스로 해야 한다(아직 기존 핸들러
//   어디에도 이 확인 지점을 넣지 않았다 - PN-0AC554C2 5단계 계속).
class AsyncTaskWaitable : public Waitable {
public:
    explicit AsyncTaskWaitable(AsyncTaskWeakRef* ref) : _ref(ref) {
        if (_ref) {
            _ref->addRef();
        }
    }

    ~AsyncTaskWaitable() override {
        if (_ref) {
            _ref->release();
        }
    }

    // 아래 struct AsyncTask 정의가 끝난 뒤(이 헤더 하단)에 정의한다 -
    // 이 시점에는 AsyncTask가 아직 전방 선언(불완전 타입)이라
    // task->state/cancelSource에 접근할 수 없다.
    bool isCompleted() const override;
    bool cancel(Task*, WaitCancelReason) override;

private:
    AsyncTaskWeakRef* _ref;
};

struct AsyncTask {
    // [갱신, 2026-09-20, PN-81E49523 2단계] context_switch.S의
    // kContextSwitch/kTaskStartTrampoline이 이 오프셋(항상 첫 필드)을
    // 그대로 참조한다 - task.h의 Task::tcb와 동일한 불변조건/타입
    // (TaskTcb=InterruptFrame, 별도 변환 없이 kContextSwitch에 바로
    // 넘길 수 있어야 함).
    TaskTcb* tcb = nullptr;

    uint64_t stackBase = 0;  // GenericSlabAllocator가 준 가상주소(해제 시 필요)
    AsyncTaskState state = AsyncTaskState::Ready;

    // [PN-C62F7908, SP-F682B889 §3.2/§7.3] 코루틴 모드 전용 - onExec()가
    // 코루틴으로 구현돼 co_await로 suspend된 경우에만 값이 채워진다
    // (스택풀 모드 또는 co_await 없이 끝까지 실행된 코루틴은 비어
    // 있음, `AsyncExecCoro::done()`이 이미 true이므로 저장할 필요가
    // 없다 - stackBase/tcb가 스택풀 모드 전용이듯 이 필드는
    // 정확히 "코루틴이 아직 끝나지 않고 남아 있을 때"만 유효하다).
    // `AsyncReactor::drainOnce()`가 이 필드로 재개 방식을 고른다 -
    // 비어 있으면 기존 `kContextSwitch`(스택풀), 채워져 있으면
    // `coroHandle.resume()`(코루틴, kContextSwitch 없이 직접 호출).
    std::coroutine_handle<> coroHandle;

    AsyncTaskSubjectCode subjectCode = 0;
    AsyncTaskManageCode manageCode = 0;
    void* args = nullptr;  // 생성/해제 전부 해당 AsyncTaskHandler 책임(SP-F682B889 §3.1)

    AtomicPtr<AsyncTask> next;  // AsyncReactor 실행 큐용 침습적 다음-포인터

    // 이 AsyncTask가 Completed/Failed에 도달하면 리액터가 이 Task를
    // scheduleImmediate로 깨워 준다(설정돼 있으면) - 일반 kernel::Task
    // 하나가 "이 특정 AsyncTask가 끝날 때까지 블로킹"하고 싶을 때 쓰는
    // 범용 훅이다(Syscall 서브시스템의 waitForSyscall이 첫 소비자).
    // 완료 시점의 코어(=이 AsyncTask를 실행한 그 코어)에서 깨우므로,
    // 대기하는 Task도 반드시 같은 코어에서 Scheduler::parkCurrent()로
    // 잠들어 있어야 한다(v1 범위 - 코어 간 이관 없음).
    //
    // [수정, 2026-09-17, PN-B4987BF6, DC-21647E46 로드맵 Phase 3
    // QU-D8FE566E 답변("진행 - 안전성이 우선")] `Task*`(관찰 포인터)에서
    // `WeakPtr<Task>`로 전환 - 등록해 둔 대기자가 이 AsyncTask 완료
    // 전에 강제 종료돼 UserThread 슬랩이 먼저 반납되는 경우(신호 기반
    // 강제 종료 등, PN-40E976F2의 onCancel 경로) 이 필드가 그 사실을
    // 모른 채 이미 해제된 메모리를 깨우려는 잠재적 UAF를 막는다 -
    // `.lock()`이 실패하면(대상이 이미 release()됨) 조용히 깨우기를
    // 건너뛴다.
    WeakPtr<Task> waitingTask;

    // [신규, 2026-09-22, PN-6EDED542, SP-F682B889 §9.5-3 - QU-FF7044DA가
    // 실측으로 드러낸 공백 해소] `waitingTask`(진짜 kernel::Task용)의
    // 코루틴 버전 - 다른 AsyncTask(코루틴이든 스택풀이든)가 `co_await
    // AsyncTaskCoroAwaiter(this)`로 이 AsyncTask의 완료를 기다리고
    // 있으면, `AsyncTaskCoroAwaiter::await_suspend()`가 자신의
    // `ensureWeakRef()`로 얻은 약한 참조를 여기 심어 둔다 - 이
    // AsyncTask가 Completed/Failed/Cancelled에 도달하면(async_task.cpp의
    // drainOnce()) `.lock()`으로 그 대기자를 찾아 `AsyncReactor::
    // submitCompletion()`으로 깨운다(코루틴이면 drainOnce()가 저장된
    // coroHandle을 재개, 스택풀이면 기존 kContextSwitch 재개 경로
    // 그대로 - 어느 쪽이든 이미 있는 재개 메커니즘 그대로 재사용).
    // `waitingTask`와 마찬가지로 대기자가 먼저 죽을 수 있어(스택풀
    // AsyncTask가 강제 취소되는 경우) 원시 포인터가 아니라 약한
    // 참조 컨트롤 블록(`AsyncTaskWeakRef`)을 통해서만 참조한다.
    AsyncTaskWeakRef* waitingAsyncTask = nullptr;

    // [신규, 2026-09-17, PN-DB5153B6, DC-21647E46 QU-B9683320 답변("(B)
    // AsyncTask에 제출자 정보를 범용화")] 이 AsyncTask를 실제로 제출한
    // kernel::Task(대개 UserThread) - `onExec()`은 `AsyncReactor`가
    // 나중에(리액터 자신의 스택 위에서) 실행하므로, 그 안에서 새로
    // `Scheduler::currentTask()`를 부르면 제출자가 아니라 그 순간
    // 리액터의 컨텍스트를 반환한다(`address_space.h`의 `MmapArgs::
    // process` 필드 주석이 이미 지적해 뒀던 문제, PN-9CC66142가 실제로
    // 처음 마주침). `Syscall::submit()`이 `Scheduler::currentTask()`가
    // 아직 정확한 시점(pendingSyscalls 부기 근처)에 이 필드를 자동으로
    // 채운다 - 개별 syscall 핸들러는 이 필드를 신경 쓸 필요 없이
    // `onExec()`에서 `task->submitterTask.lock()`으로 그냥 얻는다.
    // `waitingTask`와 동일한 이유로 WeakPtr 기반(제출자가 완료 전에
    // 먼저 죽어도 안전하게 빈 값을 반환 - UAF 방지, PN-B4987BF6의
    // self-ref 패턴이 이미 검증한 것과 같은 종류의 안전성). `Syscall::
    // submit()`을 거치지 않는 제출 경로(`Syscall::submitDetached()`,
    // 프레임워크 내부 재제출 등)는 이 필드를 채우지 않는다 - 그런
    // 경로는 애초에 "제출자"라는 개념이 아직 필요해진 적이 없다
    // (RM-23F4B687 §4, 실사용처가 생기면 그때 확장).
    //
    // [갱신, 2026-09-20, PN-C536F352] 타입을 `WeakPtr<Task>`에서
    // `TaskOwnerRef`(task.h)로 전환 - 이 필드 자체가 바로 그 타입이
    // 일반화한 "애드혹 선례"였다(TaskOwnerRef 문서 주석 참고).
    // `TaskOwnerRef::lock()`이 `WeakPtr::lock()`과 동일하게 동작하는
    // 호환 별칭이라 기존 `submitterTask.lock()` 호출부(channel.cpp/
    // debug_session.cpp/process.cpp 등 전체)는 전혀 안 건드려도 그대로
    // 컴파일된다 - 이번 전환은 대입 지점 두 곳(초기화/리셋)만 바뀐다.
    TaskOwnerRef submitterTask;

    // false면 완료(Completed/Failed) 후에도 리액터가 이 AsyncTask
    // 구조체/전용 스택을 자동으로 반납하지 않는다 - 결과를 나중에
    // 소비해야 하는 호출부(예: waitForSyscall)가 직접 반납할 책임을
    // 진다. 기본값 true(기존 "제출하고 잊는" 소비자와 동일하게 자동
    // 정리)라 기존 submit() 호출부의 동작은 그대로 유지된다.
    bool autoFree = true;

    // [PN-622BA93C/QU-FDB32CCE, 설계자 답변 2026-09-16] 이 AsyncTask가
    // 처음 제출된(init()이 호출된) 코어 - AsyncReactor::submitCompletion()
    // 이 호출자 자신의 현재 코어가 아니라 **이 필드**를 기준으로 큐잉
    // 대상 코어를 정한다("v1 - 코어 간 이관 없음" 불변식을 실제로
    // 강제하는 자리). init()이 Scheduler::currentCoreIndex()로 채운다.
    uint32_t homeCoreIndex = 0;

    // 기본값 false면 submitCompletion()이 항상 homeCoreIndex로
    // 라우팅한다(엄격한 "코어 간 이관 없음"). true로 설정하면 이
    // AsyncTask는 어느 코어에서 재개되든 상관없다는 뜻이라
    // submitCompletion()이 호출자 자신의 현재 코어에 그대로 큐잉한다
    // (설계자 답변 옵션 2 - 코어 친화성이 필요 없는 AsyncTask를 위한
    // 옵트인 완화). 호출부가 이 AsyncTask의 코드가 실제로 코어
    // 로컬 상태(gCurrentAsyncTask[coreIndex] 등)에 의존하지 않음을
    // 스스로 보장해야 한다.
    bool allowCoreMigration = false;

    // §8 협조적 취소 채널 - onExec/코루틴 몸체가 cancelSource.token()
    // 으로 얻은 AsyncToken을 원하는 지점마다 확인한다. 트리거 지점은
    // §8.3 - (1) 소유자(UserThread) 조기 종료(scheduler.cpp의
    // SelfTerminateHandler::onExec), (2) SP-0666DB3C §9 Waitable::
    // cancel() 경로(아직 미연동 - PN-71E50394 Signal 인프라와 함께
    // 후속), (3) 타임아웃(PN-D01B7D07, QU-681F256C 설계자 답변 -
    // "Cancel Source 쪽에 timeout을 유발").
    AsyncTokenSource cancelSource;

    // [PN-D01B7D07, §3.7] `ensureWeakRef()`(아래)가 처음 호출된 적
    // 있으면 그때 만들어진 약한 참조 컨트롤 블록 - 이 AsyncTask가
    // 실제로 반납될 때(kReleaseAsyncTask, async_task.cpp) 이 필드를
    // 보고 무효화/해제한다(AsyncTask 자신의 몫 하나만). 아직 아무도
    // 요청한 적 없으면 계속 nullptr. [갱신, 2026-09-18, PN-0EB2FABF]
    // 이제 타임아웃(`scheduleTimeout()`) 전용이 아니다 - `Join`(process.cpp)
    // 도 같은 컨트롤 블록을 공유해 쓴다(둘 다 걸려 있어도 안전 -
    // `ensureWeakRef()`가 이미 있으면 그대로 재사용).
    AsyncTaskWeakRef* weakRef = nullptr;

    // [신규, 2026-09-19, PN-0AC554C2/PN-EA968DF0, QU-B89531F0 답변]
    // `ensureWaitable()`(아래)이 처음 호출된 적 있으면 그때 만들어진
    // 얇은 `Waitable` 래퍼 - AsyncTask 자신이 이 `SharedPtr`을 강하게
    // 소유하고 있다가, 이 AsyncTask가 실제로 반납될 때(암묵적으로
    // `SharedPtr` 소멸자가) 함께 해제한다. 순환 참조 없음
    // (`AsyncTaskWaitable`은 `weakRef`를 통해 이 AsyncTask를 약하게만
    // 참조). 아직 아무도 요청한 적 없으면 계속 빈 `SharedPtr`.
    SharedPtr<AsyncTaskWaitable> selfWaitable;

    // [신규, 2026-09-18, PN-0EB2FABF] `scheduleTimeout()`이 이미 이
    // AsyncTask에 타임아웃을 건 적 있는지 - `weakRef != nullptr`과는
    // 이제 별개 축이다(weakRef는 Join도 만들 수 있으므로, "weakRef가
    // 있다"가 더 이상 "타임아웃이 걸려 있다"를 뜻하지 않는다). v1
    // "타임아웃은 AsyncTask당 최대 한 번"이라는 기존 불변조건은 이
    // 플래그 하나로 유지한다.
    bool timeoutScheduled = false;

    // [신규, 2026-09-18, SP-76250478 §3.1, PN-0EB2FABF] `weakRef`가
    // 아직 없으면 새로 만들어(AsyncTask 자신의 몫으로 `init()`) 채우고,
    // 있으면 그대로 반환(멱등, `ensureSelfRef()`류 관례와 동일) - 실패
    // (슬랩 고갈) 시 nullptr. **호출부는 반환값이 non-null이면 자신의
    // 몫만큼 직접 `addRef()`를 불러야 한다** - 이 함수 자체는 관찰자
    // 등록을 하지 않는다(순수 "블록을 얻기/만들기"만 담당).
    AsyncTaskWeakRef* ensureWeakRef();

    // [신규, 2026-09-19, PN-0AC554C2/PN-EA968DF0, QU-B89531F0 답변
    // ("제안대로 진행")] `selfWaitable`(아래)이 아직 없으면 `kMakeShared
    // <AsyncTaskWaitable>(ensureWeakRef())`로 새로 만들어 채우고, 있으면
    // 그대로 반환(멱등, `ensureWeakRef()`와 동일한 관례) - 실패(슬랩
    // 고갈, 또는 `ensureWeakRef()` 자체 실패) 시 빈 `SharedPtr`.
    // 반환값을 `Task::blockedOn`에 `WeakPtr<Waitable>`로 아일리어싱해
    // 넣는 게 이 함수의 유일한 존재 이유다(`WaitQueue`가 자신을 담은
    // Mutex/Semaphore의 컨트롤 블록을 빌려 쓰는 것과 동형 - 다만
    // 여기서는 AsyncTask 자신이 그 "담아 주는 그릇"이다).
    SharedPtr<AsyncTaskWaitable> ensureWaitable();

    // [PN-D01B7D07, SP-F682B889 §3.7] delayTicks(Timer::tickCount()
    // 단위, DelayedExecutionQueue 재사용) 뒤에도 이 AsyncTask가 아직
    // 끝나지 않았으면 cancelSource.trigger()로 취소(§8.3 세 번째
    // 트리거 지점, QU-681F256C 답변 그대로) - 이미 끝나 반납됐으면
    // (약한 참조가 무효화돼 있으면) 아무 일도 하지 않는다. 한
    // AsyncTask에 최대 한 번만 호출한다(v1 - 두 번째 호출은 무시).
    void scheduleTimeout(uint64_t delayTicks);

    // subjectCode에 등록된 AsyncTaskHandler::onExec을 처음 실행할
    // 준비가 된 상태로 스택을 구성한다(GenericSlabAllocator에서 전용
    // 스택을 확보) - 실패 시(할당 고갈) stackBase가 0으로 남는다,
    // 호출부(submit)가 반드시 확인해야 한다.
    void init(AsyncTaskSubjectCode subjectCode, AsyncTaskManageCode manageCode, void* args);

    // 현재 실행 중인 AsyncTask 자신이 호출 - 리액터 컨텍스트로 복귀해
    // 다음 AsyncTask를 처리하게 한다. 나중에 리액터가 이 AsyncTask를
    // 다시 큐에서 뽑아 재개시키면 이 호출 지점부터 이어진다.
    static void yield();

    // [신규, 2026-09-22, PN-4D60D49C, SP-0666DB3C §15.2] "지금 이
    // 코어에서 실행/재개 중인 AsyncTask" - async_task.cpp의
    // `gCurrentAsyncTask[coreIndex]`(AsyncReactor::drainOnce()가 코루틴
    // resume()/kContextSwitch 진입 직전·직후에만 갱신하는 파일-로컬
    // 배열, PN-622BA93C 이전부터 이미 존재)를 그대로 조회해 반환한다 -
    // 이 값은 이미 정확히 SP-0666DB3C §15.2/SP-F682B889 §3.4가 확정한
    // "gCurrentAsyncTask"의 의미와 갱신 지점을 그대로 만족하고 있어
    // (실측 검증된 기존 코드), 문서가 제안한 `TaskLocal<AsyncTask*>`
    // 래퍼 타입으로 다시 감싸지 않고 이 접근자 하나만 새로 노출한다
    // (RM-23F4B687 "검증된 코드는 순수 리팩터링 목적만으로 건드리지
    // 않는다" 원칙 - 이 §15.2 자신이 §11의 ThreadLocal에 대해 이미
    // 선언한 것과 동일한 판단). 리액터/idle 컨텍스트 자신에서 부르면
    // (지금 실행 중인 AsyncTask가 없으므로) nullptr.
    static AsyncTask* current();

    // AsyncTask/AsyncCallbackRegistry가 내부적으로 새 AsyncTask를 만들어
    // 등록하고 이 코어의 리액터에 제출하는 진입점 - 실패 시(구조체
    // 또는 전용 스택 확보 실패) nullptr. autoFree=false로 제출하면
    // 리액터가 완료 후에도 반납하지 않는다(호출부가 나중에 결과를
    // 읽고 직접 반납해야 함 - waitForSyscall류의 소비 패턴).
    //
    // [신규, 2026-09-18, PN-4FA5F13B 근본 원인 수정] `preemptive`는
    // 그대로 `AsyncReactor::submitCompletion()`에 전달된다(기본값
    // false는 기존 동작 그대로 - 이 코어가 다음 idle 분기에서 자연히
    // 드레인). **`Syscall::submit()`처럼 제출자가 실제로
    // `Scheduler::parkCurrent()`로 블로킹할 수 있는 경로는 반드시
    // true를 넘겨야 한다** - `Scheduler::runLoop()`이 `pickNext()`를
    // `AsyncReactor::drainOnce()`보다 먼저 확인하므로(scheduler.cpp),
    // 이 코어에 계속 Ready 상태인 다른 Task(예: SpawnProcess로 막
    // 스폰된, syscall을 전혀 안 쓰는 CPU-bound 자식)가 있으면 그
    // Task가 계속 뽑히는 한 idle 분기 자체에 영원히 도달하지 못해
    // drainOnce()가 무기한 굶는다(실측 확인 - 같은 호출자의 두 번째
    // SpawnProcess부터 재현되던 "wait()가 영원히 안 깨어남" 버그의
    // 진짜 원인). preemptive=true는 `Lapic::sendFixedIpi()`로 그 코어에
    // `kAsyncDrainVector` IPI를 걸어 인터럽트 컨텍스트에서 강제로
    // drainOnce()를 돌게 만들어(async_task.h `submitCompletion` 문서
    // 참고, 인터럽트 컨텍스트 호출 안전성 이미 문서화됨) 이 굶주림을
    // 근본적으로 없앤다. vfs_syscall.cpp의 내부 KernelDriver 디스패치처럼
    // `AsyncTaskAwaiter`/`AsyncTaskGroup::pendingCount()`로 능동
    // 폴링(`yieldCurrent()` 기반 - 정상적인 라운드로빈으로 언젠가 다시
    // 스케줄되므로 이 굶주림에 애초에 안 걸림)하는 호출부는 그대로
    // 기본값(false)이면 충분하다 - 불필요한 IPI 비용을 늘리지 않는다.
    static AsyncTask* submit(AsyncTaskSubjectCode subjectCode, AsyncTaskManageCode manageCode, void* args,
                             bool autoFree = true, bool preemptive = false);
};

// [PN-D01B7D07] 이 AsyncTask를 실제로 반납하는 유일한 통로(정의는
// async_task.cpp) - 아래 `AsyncTaskCoroAwaiter::await_resume()`처럼
// 헤더에 인라인으로 정의되는 코드(여러 번역 단위에서 쓰이므로 인라인
// 이어야 함, ODR)도 이 함수를 참조해야 해서 여기 전방 선언한다.
void kReleaseAsyncTask(AsyncTask* task);

// AsyncTaskWaitable::isCompleted()/cancel() 정의 - struct AsyncTask가
// 이제 완전한 타입이라 여기서만 task->state/cancelSource에 접근할 수
// 있다(클래스 선언 자체는 위 AsyncTaskWeakRef 바로 뒤, AsyncTask보다
// 앞에 있다 - AsyncTask::selfWaitable 필드가 이 타입을 완전한 상태로
// 필요로 하기 때문).
inline bool AsyncTaskWaitable::isCompleted() const {
    AsyncTask* task = _ref ? _ref->lock() : nullptr;
    if (!task) {
        return true;
    }
    return task->state == AsyncTaskState::Completed || task->state == AsyncTaskState::Failed ||
           task->state == AsyncTaskState::Cancelled;
}

inline bool AsyncTaskWaitable::cancel(Task*, WaitCancelReason) {
    AsyncTask* task = _ref ? _ref->lock() : nullptr;
    if (!task) {
        return false;
    }
    task->cancelSource.trigger();
    return true;
}

// 작업 주체(기능)별로 구현 - 실행/실패/취소 셋 다 구현 책임을 진다.
// args(작업 주체별 내부 데이터)를 실행 전에 만드는 것도, 실행이
// 끝나거나(onExec) 실패하거나(onFailure) 소유자가 먼저 죽어
// 취소되거나(onCancel) 어느 쪽으로 끝나든 그 해제도 전부 이
// 처리기의 몫이다 - 프레임워크는 관여하지 않는다.
class AsyncTaskHandler {
public:
    virtual ~AsyncTaskHandler() = default;
    // [PN-C62F7908, SP-F682B889 §7.3, 설계자 지시 2026-09-16 - "C++
    // 코루틴은 가상함수를 지원하는 걸로 알고 있는데, 가상함수 정의를
    // 코루틴으로 바꿔"] 코루틴 전용 별도 가상 메서드를 두지 않고
    // onExec 자신의 반환 타입을 `AsyncExecCoro`로 바꿨다 - 본문에
    // `co_await`/`co_return`을 안 쓰면 그냥 즉시 완료되는 평범한
    // 함수처럼 동작한다(단, `co_return;`은 반드시 있어야 한다 -
    // 그래야 컴파일러가 이 함수를 코루틴으로 변환한다, C++20 규칙).
    virtual AsyncExecCoro onExec(AsyncTask* task, void* args) = 0;
    // Slab 할당 실패(nullptr) 등 프레임워크 내부 사유를 포함해
    // 실행 자체가 불가능했을 때 호출된다 - 재시도 여부도 이 처리기가
    // 결정한다(예: 여기서 다시 submit).
    virtual void onFailure(AsyncTask* task) = 0;
    // 이 작업의 소유 스레드/프로세스가 완료 전에 종료돼 강제로
    // 취소될 때 호출된다(onExec/onFailure 둘 다와 배타적). **[구현
    // 완료, PN-40E976F2]** 이 UserThread가 종료될 때
    // (scheduler.cpp의 SelfTerminateHandler::onExec) 아직 완료/실패
    // 전인 pendingSyscalls 항목을 전부 AsyncTaskState::Cancelled로
    // 전이시키고, AsyncReactor::reactorTaskEntry가 그 상태를 보면
    // onExec 대신 이 메서드만 호출한다 - args의 해제도 이 호출
    // 안에서 처리기가 직접 책임진다(onExec/onFailure와 동일한 계약).
    virtual void onCancel(AsyncTask* task, void* args) = 0;
};

// 기능별로 부팅/초기화 시 한 번만 등록한다 - 요청이 생길 때마다
// 등록/해제하지 않는다.
class AsyncCallbackRegistry {
public:
    static AsyncTaskSubjectCode registerHandler(AsyncTaskHandler* handler);
    static AsyncTaskHandler* resolve(AsyncTaskSubjectCode subjectCode);
};

// SP-F682B889 §3.3(QU-86DD998F 설계자 답변, 2026-09-16) - "AsyncTaskGroup은
// AsyncTask를 확장하는 무언가가 아니라 그 작업들을 대기하는 곳에서
// 모아놓고 관리하는 유틸리티야. 얘는 AsyncTask를 weak ref로 들고
// 있으면 돼." `AsyncTask` 구조체 자체는 전혀 건드리지 않는다 - 이
// 그룹이 담는 포인터는 소유권이 없는 약한 참조라, 각 멤버는 반드시
// **`autoFree=false`로 제출**돼야 한다(`Syscall::submit()`이
// `AsyncTask::submit(..., autoFree=false)`로 자기 소비를 예약하는
// 것과 동일한 관례 - 소유권/수명 관리는 여전히 제출자 쪽에 있고,
// 이 그룹은 그 수명 위에 얹혀 상태만 들여다본다). 완료 여부를 알려줄
// 콜백/필드가 없으므로(그런 훅을 AsyncTask에 추가하지 않기로 확정
// 됐으므로) 폴링으로 확인한다.
class AsyncTaskGroup {
public:
    // task는 autoFree=false로 제출된 것이어야 한다(위 클래스 문서
    // 참고) - 이 그룹이 나중에 pendingCount()/완료 확인 중에 직접
    // 반납한다(Syscall::waitForAnyOf의 readyTask 반납과 동일한 관례).
    void add(AsyncTask* task);

    // 아직 끝나지 않은(Completed/Failed/Cancelled가 아닌) 멤버 수.
    // 호출할 때마다 이미 끝난 멤버를 찾아 이 자리에서 직접 정리한다
    // (스택/구조체 반납 - "폴링하면서 동시에 청소"가 이 유틸리티의
    // 유일한 소비 경로이므로 별도 consume API를 두지 않는다).
    uint32_t pendingCount();

private:
    // §6-4/PN-BCE6CFF3(QU-F475C6C2 답변)와 동일한 관례 - 단순 배열
    // 대신 재사용 가능한 제네릭 컨테이너.
    static constexpr uint32_t kChunkCapacity = 8;
    ChunkedList<AsyncTask*, kChunkCapacity> _tasks;
};

// 그룹 전체가 끝날 때까지 "현재(진짜 kernel::Task) 컨텍스트"를
// 되풀이해 확인한다. **설계 변경(실측 반영, 2026-09-16)**: §3.3
// 원안은 "스케줄러의 비공개 block 프리미티브"(Scheduler::parkCurrent
// 류의 진짜 파킹)를 제안했으나, QU-86DD998F가 확정한 weak-ref-폴링
// 모델에는 그 파킹을 깨워 줄 콜백/통지 경로가 없다(그런 훅을
// AsyncTask에 추가하지 않기로 확정됐으므로) - 그래서 대신
// `Scheduler::yieldCurrent()`(진짜 블로킹이 아니라 협조적 양보) 반복
// 으로 구현한다. 코어를 완전히 놓지는 않지만(스케줄러 틱마다 다시
// 깨어나 재확인), 다른 Ready Task들에게는 계속 실행 기회를 준다.
class AsyncTaskWaitGroup {
public:
    void add(AsyncTask* task) { _group.add(task); }
    void waitAll();

private:
    AsyncTaskGroup _group;
};

// 다른 AsyncTask 하나가 끝나기를 "현재 AsyncTask 컨텍스트 안에서"
// 기다린다 - AsyncTask::yield()를 반복 호출하며 리액터에 제어를
// 돌려주는 방식으로 구현한다(전체 리액터를 블로킹하지 않음). target
// 도 위 AsyncTaskGroup과 동일하게 autoFree=false로 제출된 것이어야
// 하며, 완료를 관측한 이 호출이 직접 반납한다.
//
// **[정정, 2026-09-22, PN-6EDED542/QU-FF7044DA 실측]** 이 클래스는
// **스택풀 AsyncTask 컨텍스트 전용**이다 - `AsyncTask::yield()`가
// `kContextSwitch`로 이 AsyncTask 자신의 전용 스택(`tcb`)으로/에서
// 되돌아가는 방식이라, `onExec()`이 코루틴(`AsyncExecCoro`, C++
// `co_await`)으로 구현된 경우엔 애초에 그 전용 스택 위에서 실행되는
// 게 아니라 `drainOnce()`의 C++ 호출 스택 위에서 직접 실행되므로
// `kContextSwitch`로 돌아갈 지점 자체가 없다 - 코루틴 `onExec()`
// 안에서 이 클래스(정확히는 `.await()`)를 부르면 실측으로 확인된
// 무한 대기가 발생한다(`Ext4Driver` 구현 중 QEMU에서 재현). 코루틴
// 안에서는 대신 아래 `AsyncTaskCoroAwaiter`를 `co_await`로 쓴다.
class AsyncTaskAwaiter {
public:
    explicit AsyncTaskAwaiter(AsyncTask* target) : _target(target) {}
    void await();

private:
    AsyncTask* _target;
};

// [신규, 2026-09-22, PN-6EDED542, SP-F682B889 §9.5-3] 위
// `AsyncTaskAwaiter`의 코루틴 버전 - `process.cpp`의 `JoinAwaiter`
// (`SP-76250478` §3.1, 이미 실사용 중인 검증된 선례)를 범용화했다.
// `KernelFsDriver::onExec()`처럼 `AsyncExecCoro`로 구현된 코루틴
// 안에서 다른 AsyncTask(전형적으로 `BlockDevice::submitReadBlocks()`
// 가 돌려준 I/O 완료 AsyncTask)의 완료를 기다릴 때 `co_await
// AsyncTaskCoroAwaiter(target)`로 쓴다 - awaiter 프로토콜(await_ready/
// await_suspend/await_resume)을 구현해 C++20 코루틴 규격 그대로
// 동작한다. `target`은 `AsyncTaskGroup`/`AsyncTaskAwaiter`와 동일하게
// autoFree=false로 제출된 것이어야 하며, `await_resume()`이 직접
// 반납한다.
class AsyncTaskCoroAwaiter {
public:
    explicit AsyncTaskCoroAwaiter(AsyncTask* target) : _target(target) {}

    bool await_ready() const noexcept {
        return _target == nullptr || _target->state == AsyncTaskState::Completed ||
               _target->state == AsyncTaskState::Failed || _target->state == AsyncTaskState::Cancelled;
    }

    // true를 반환하면 실제로 정지(나중에 target 완료 시 drainOnce()가
    // 재개), false면 즉시 재개(슬랩 고갈 등 - await_resume()이 그
    // 시점의 target->state를 그대로 반환하므로 호출부는 여전히 정확한
    // 상태를 관측한다, 다만 target이 아직 안 끝났는데도 재개된다는
    // 뜻이라 호출부가 재시도/폴링 여지를 남겨 둬야 한다).
    bool await_suspend(std::coroutine_handle<>) noexcept {
        AsyncTask* self = AsyncTask::current();
        if (!self) {
            return false;  // 코루틴 onExec 밖에서 잘못 호출된 경우 - 정지하지 않고 곧장 재개
        }
        AsyncTaskWeakRef* ref = self->ensureWeakRef();
        if (!ref) {
            return false;  // 슬랩 고갈 - JoinAwaiter와 동일 관례(정지 없이 즉시 재개)
        }
        ref->addRef();  // "target->waitingAsyncTask" 몫 - drainOnce()가 소비 후 release()
        _target->waitingAsyncTask = ref;
        return true;
    }

    AsyncTaskState await_resume() const noexcept {
        const AsyncTaskState result = _target->state;
        kReleaseAsyncTask(_target);
        return result;
    }

private:
    AsyncTask* _target;
};

// 커널 전용 비동기 프레임워크의 디스패치 계층(SP-F682B889 §3.4/§4,
// 2026-09-16 재구조 - QU-96BBB769/QU-4034561A/QU-3BDEE348 답변,
// PN-FEAAF154) - **더 이상 코어당 전용 kernel::Task가 아니다.** 예전
// (PN-C46DF296까지)에는 리액터가 각 코어에 하나씩 배치되는 전용
// Task로 존재했으나, "리액터가 idle을 흡수한다"는 설계자 지시에 따라
// `Scheduler::runLoop()`의 idle 폴백(코어에 실행할 Task가 없을 때)이
// `drainOnce()`를 직접 호출하는 방식으로 전면 교체됐다 - 새 kernel::
// Task/전용 스택/park-wake 프로토콜이 전혀 없다.
class AsyncReactor {
public:
    // 전역 1회(BSP에서만, Idt::init() 이후) - "다른 Task가 실행
    // 중일 때"의 즉시 개입 경로(§4 (C), async_task.cpp의
    // kAsyncDrainVector) IDT 벡터를 등록한다. 코어별 상태는 전혀
    // 없다(gExecQueues/gPreemptiveQueues는 이미 정적 배열) - AP는
    // 더 이상 이 클래스를 위해 아무것도 호출할 필요가 없다.
    static void init();

    // 이 코어의 실행 큐(선점 큐 우선)에서 정확히 하나를 꺼내 실행/
    // 재개하거나(kContextSwitch), 만료된 지연 타이머(DelayedExecutionQueue,
    // SP-F15B4A63)를 처리한다 - 할 일을 하나 처리했으면 true, 정말
    // 아무 것도 없었으면 false(그 결과에 따라 호출부가 계속 반복할지
    // 결정한다). **재진입 방지**: 이미 이 코어에서 드레인이 진행 중이면
    // (runLoop()의 인라인 호출이든 §4 (C) 경로의 IPI 핸들러든) 즉시
    // false를 반환한다 - 그렇지 않으면 AsyncTask의 kContextSwitch
    // 재개 지점(gReactorSavedRsp[coreIndex])을 두 실행 흐름이 동시에
    // 덮어쓸 수 있다(같은 코어 안에서 인터럽트로만 발생 가능한 중첩 -
    // 새로 큐잉된 항목은 바깥쪽에서 이미 진행 중인 호출이 이어서
    // 처리하므로 유실되지 않는다). `Scheduler::runLoop()`의 idle
    // 분기와 §4 (C)의 IPI 핸들러(kAsyncDrainVector) 둘 다 이 함수를
    // 호출한다.
    static bool drainOnce(uint32_t coreIndex);

    // 인터럽트 컨텍스트에서 호출 가능 - 완료(또는 새로 생성)된
    // AsyncTask를 **그 task의 큐잉 대상 코어**(아래 참고)의 실행
    // 큐에 push한다.
    //
    // **[재설계, 2026-09-16, QU-3BDEE348 답변 - 하이브리드 (A)+(C)]**
    // 리액터가 더 이상 Task가 아니므로 "파킹돼 있으면 강제 스케줄링"
    // 개념 자체가 사라졌다 - 대신:
    // - **preemptive=false → (A)**: 그냥 큐에 넣기만 한다. 그 코어가
    //   다음에 idle 분기(pickNext()==nullptr)에 도달하는 순간 자연히
    //   처리된다(그 사이 다른 Task가 실행 중이었다면 그 Task가 곧
    //   블로킹되거나 스스로 끝나 idle로 돌아오는 게 일반적인 패턴 -
    //   실측 근거: `Syscall::submit()`이 유일한 "다른 Task 실행 중"
    //   실사용처이고, 그 직후 관례상 `Syscall::wait()`로 곧 자기
    //   자신을 파킹한다).
    // - **preemptive=true → (C)**: 선점 큐에 넣은 뒤 그 코어에게
    //   `kAsyncDrainVector` IPI를 보낸다(async_task.cpp) - 인터럽트가
    //   다시 켜지는 즉시 그 ISR이 `drainOnce()`를 큐가 빌 때까지
    //   반복 호출해 즉시 처리를 보장한다. exclusivePreemptive
    //   Channel(SP-00CA7175 Tier B) 핸드셰이크 완료 등 지연시간이
    //   중요한 경로가 이 값을 넘긴다.
    //
    // **[수정, 2026-09-16, PN-622BA93C/QU-FDB32CCE]** 큐잉 대상 코어는
    // 더 이상 항상 "호출자 자신의 현재 코어"가 아니다 - `task->
    // allowCoreMigration`이 false(기본값)면 `task->homeCoreIndex`로
    // 라우팅한다(호출자와 다른 코어면 그 코어에 IPI를 보낸다 - 이
    // 함수 자신이 이미 인터럽트 컨텍스트에서도 안전하다고 문서화돼
    // 있어 cross-core self-IPI가 아닌 일반 IPI를 보내는 것도 안전,
    // Push/Pull 로드밸런싱의 kWakeCoreIfIdle과 같은 패턴). true면
    // (설계자 답변 옵션 2 - 코어 친화성 불필요를 스스로 보장하는
    // AsyncTask) 그냥 호출자 자신의 현재 코어에 큐잉한다(기존 동작).
    // 채널 핸드셰이크(channel.cpp)처럼 서로 다른 코어의 AsyncTask가
    // 서로에게 completion을 보내는 협조적 패턴이 바로 이 라우팅이
    // 필요했던 실측 사례(PN-622BA93C, Push/Pull 로드밸런싱으로
    // accepter/connector가 다른 코어에 배치되며 처음 드러남).
    static void submitCompletion(AsyncTask* task, bool preemptive = false);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_ASYNC_TASK_H
