#include "debug_session.h"

#include "idt.h"
#include "interrupt_frame.h"
#include "process.h"
#include "scheduler.h"

namespace kernel {

bool kIsPausedByDebugger(Task* task) {
    if (!task->isUserLevel) {
        return false;
    }
    auto* thread = static_cast<UserThread*>(task);
    SharedPtr<Process> proc = thread->process.lock();
    if (!proc) {
        return false;
    }
    return proc->debugSession.pausedByDebugger;
}

namespace {

// [SP-9A6D579F §3.2] 권한 모델 - **이번 증분은 "직계 부모" 경로만
// 구현한다.** 원안은 (a) 호출자가 ProcessRole::KernelService면
// 부모-자식 여부와 무관하게 임의 프로세스를 대상으로 허용하는
// 예외도 두지만, 그러려면 "임의의 targetProcessId를 검증 없이
// 안전하게 대상 Process*로 되돌리는 방법"이 필요한데 - 이건
// `Kill`(signal.h `KillArgs` 문서 주석, `QU-764C5624`)이 이미 마주쳐
// 아직 설계자 답변을 기다리고 있는 바로 그 미해결 질문과 정확히
// 같다(임의 포인터 역참조 보안 공백). 그 답이 나오기 전까지
// KernelService 예외는 구현하지 않는다 - `Kill`과 동일하게 "호출자
// 자신의 직계 자식만" 대상으로 허용한다(`WaitHandler`/`KillHandler`
// 와 동일한 스코프/검증 방식). `QU-764C5624`가 답변되면 그 정책을
// 이 함수에도 그대로 반영한다.
SharedPtr<Process> kFindDebuggableChild(const SharedPtr<Process>& caller, int64_t targetProcessId) {
    SharedPtr<Process> target;
    caller->children.forEach([&](SharedPtr<Process>& child, auto*) {
        if (target || !child) {
            return;
        }
        if (reinterpret_cast<int64_t>(child.get()) == targetProcessId) {
            target = child;
        }
    });
    return target;
}

class DebugAttachHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DebugAttachArgs*>(argsRaw);

        // WaitHandler/KillHandler와 동일한 관례(PN-5BBD4301) -
        // Scheduler::currentTask()가 아니라 task->submitterTask.lock()
        // 으로 제출자를 얻는다.
        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* callerThread = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> caller = callerThread ? callerThread->process.lock() : SharedPtr<Process>();
        if (!caller) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        SharedPtr<Process> target = kFindDebuggableChild(caller, args->targetProcessId);
        if (!target) {
            // 위 kFindDebuggableChild 문서 주석 참고 - v1은 직계
            // 자식이 아니면 KernelService 여부와 무관하게 전부 거부.
            args->error = ChannelError::PermissionDenied;
            co_return;
        }

        if (target->debugSession.active) {
            // §7 영구 불변조건 - 디버기당 세션 1개(안티 디버깅 기반).
            // 이미 자기 자신이 그 세션의 소유자여도 예외 없음(재부착
            // API가 아니다 - 필요하면 먼저 DebugDetach).
            args->error = ChannelError::AlreadyExists;
            co_return;
        }

        target->debugSession = DebugSession{};
        target->debugSession.active = true;
        target->debugSession.debuggerProcess = WeakPtr<Process>(caller);
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

DebugAttachHandler gDebugAttachHandler;

class DebugDetachHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DebugDetachArgs*>(argsRaw);

        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* callerThread = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> caller = callerThread ? callerThread->process.lock() : SharedPtr<Process>();
        if (!caller) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        SharedPtr<Process> target = kFindDebuggableChild(caller, args->targetProcessId);
        if (!target) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }

        if (!target->debugSession.active) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        // 이 세션을 쥔 디버거가 바로 이 호출자인지 확인 - 대상의
        // 직계 부모라고 해서 무조건 남의 세션을 끊을 수 있는 건
        // 아니다(예: 부모가 자식 A에 Attach한 뒤 자식 B가 같은
        // 부모의 다른 자식이라 해도 A의 세션을 대신 끊을 자격은
        // 없음 - 이 경우는 애초에 target이 B라 debuggerProcess가
        // A 세션에서 A 자신을 가리키므로 caller(=부모)와 다르다는
        // 게 아니라, 애초에 "직계 부모"라는 자격 자체가 대상별로
        // 이미 검증됐다는 점에 주의 - 여기서는 "그 세션을 정말
        // 이 호출자가 열었는지"만 별도로 재확인한다, 부모가 여러
        // 자식에 Attach했다가 서로 다른 DebugDetach 호출로 섞일
        // 가능성은 없지만 방어적으로 명시).
        SharedPtr<Process> debugger = target->debugSession.debuggerProcess.lock();
        if (!debugger || debugger.get() != caller.get()) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }

        target->debugSession = DebugSession{};
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

DebugDetachHandler gDebugDetachHandler;

// [신규, 2026-09-17, SP-9A6D579F §3.4] DebugSetBreakpoint 본체 -
// DebugDetachHandler와 정확히 같은 권한 검증 패턴(§6 "호출자가 그
// pid의 활성 세션의 debuggerProcess가 아니면 PermissionDenied, 세션
// 자체가 없으면 InvalidState류" - 이 코드베이스엔 ChannelError::
// InvalidState가 없어 기존 관례대로 NotFound로 대체) - 검증을
// 공유 헬퍼로 뽑지 않은 이유는 DebugDetachHandler가 검증 직후 하는
// 일(세션 전체 초기화)과 이 핸들러가 하는 일(슬롯 하나만 갱신)이
// 갈라져 공유해봐야 몸통 자체는 안 줄어들기 때문(RM-23F4B687 §4).
class DebugSetBreakpointHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DebugSetBreakpointArgs*>(argsRaw);

        if (args->slot >= kMaxDebugBreakpoints) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }

        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* callerThread = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> caller = callerThread ? callerThread->process.lock() : SharedPtr<Process>();
        if (!caller) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        SharedPtr<Process> target = kFindDebuggableChild(caller, args->targetProcessId);
        if (!target) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }
        if (!target->debugSession.active) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        SharedPtr<Process> debugger = target->debugSession.debuggerProcess.lock();
        if (!debugger || debugger.get() != caller.get()) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }

        DebugBreakpoint& slot = target->debugSession.breakpoints[args->slot];
        slot.enabled = args->enable;
        slot.address = args->address;
        slot.condition = args->condition;
        // [중요] 여기서는 하드웨어 DR0-3/DR7에 아무것도 쓰지 않는다 -
        // 이 syscall을 호출한 스레드(디버거 자신)가 지금 이 코어에서
        // 실행 중이라 DR 레지스터를 건드리면 디버거 자신에게 영향을
        // 준다. 실제 하드웨어 반영은 대상(target->mainThread)이 다음
        // 디스패치될 때 `Scheduler::onTick()` 등이 부르는
        // `kSyncDebugRegs()`(scheduler.cpp, SP-83A07867 §3.2 네 번째
        // 훅)가 그 시점에 대상 자신의 코어에서 대신 한다 - §5가 이미
        // "다음 디스패치에서 반영, 최악의 경우 한 타임퀀텀 지연"이라고
        // 명시해 둔 그대로.
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

DebugSetBreakpointHandler gDebugSetBreakpointHandler;

// [신규, 2026-09-17, SP-9A6D579F §3.5/§4] #DB ISR(idt.cpp의
// kHandleDebugException)이 하드웨어 브레이크포인트 적중 시 부르는
// 콜백 - Idt::registerDebugCallback()으로 등록한다.
//
// **이 함수 자신은 이 Task를 당장 멈추지 못한다(중요, 정직하게
// 기록)** - #DB는 트랩이라 이 함수가 반환한 뒤 CPU는 iretq로 그대로
// ring3 실행을 재개한다(TaskState는 CPU 실행 흐름에 아무 영향을
// 주지 않는 순수 스케줄러 장부일 뿐). 대신 `pausedByDebugger`만 세워
// 두고, 실제로 "재큐잉하지 않는다"는 결정은 `Scheduler::onTick()`의
// 재스케줄 결정 지점(`kIsPausedByDebugger()`를 확인하도록 이미
// 배선됨, scheduler.cpp)이 **다음 스케줄러 틱**(최대 한 타임퀀텀,
// ~10ms)에 내린다 - `ResourceGroup::freeze()`가 이미 겪고 있는 것과
// 정확히 같은 종류의 한계(§4 "이미 Ready 상태로 대기 중이던 Task는
// 당장 멈추지 못한다")를 여기서도 그대로 받아들인다 - 이 지연을
// 없애려면 #DB ISR 자신이 IST4(고정 per-core 스택)에서 직접
// kContextSwitch로 다른 Task로 전환해야 하는데, 그건 이 세션이
// 안전하게 검증할 수 없다고 판단해 명시적으로 범위 밖으로 뺐다(다음
// 세션 후보 - "남은 범위" 참고).
bool kHandleUserBreakpointHit(InterruptFrame*, uint64_t dr6) {
    constexpr uint64_t kDr6BreakpointMask = 0xF;  // B0-B3
    if ((dr6 & kDr6BreakpointMask) == 0) {
        // BS(싱글스텝)류 - DebugSetSingleStep이 아직 미구현이라 이
        // 콜백은 하드웨어 브레이크포인트(B0-B3)만 다룬다. false를
        // 반환해 idt.cpp가 기존처럼 로그만 남기고 계속 실행하게 둔다.
        return false;
    }

    Task* task = Scheduler::currentTask();
    if (!task || !task->isUserLevel) {
        // 유저 프로세스 대상만(SP-9A6D579F §2, 커널 자체 디버깅은
        // SP-DABFCF9F의 별개 QEMU gdb stub 워크플로) - 이론상 도달
        // 불가(DR 레지스터는 kSyncDebugRegs가 유저 Task에만 실어
        // 두므로 커널 전용 Task 실행 중엔 항상 0) - 방어적으로만.
        return false;
    }
    auto* thread = static_cast<UserThread*>(task);
    SharedPtr<Process> proc = thread->process.lock();
    if (!proc || !proc->debugSession.active) {
        // 세션이 그 사이 Detach됐을 수 있음(kSyncDebugRegs가 다음
        // 디스패치에서야 DR7을 0으로 되돌리므로, 그 틈에 우연히 남아
        // 있던 하드웨어 브레이크포인트가 한 번 더 걸릴 수 있다) - 무해,
        // 그냥 계속 실행.
        return false;
    }
    proc->debugSession.pausedByDebugger = true;
    return true;
}

}  // namespace

void DebugSessionService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointDebugAttach, &gDebugAttachHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugDetach, &gDebugDetachHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugSetBreakpoint, &gDebugSetBreakpointHandler);
}

void DebugSessionService::registerDebugCallback() {
    Idt::registerDebugCallback(&kHandleUserBreakpointHit);
}

}  // namespace kernel
