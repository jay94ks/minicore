#include "debug_session.h"

#include "process.h"
#include "scheduler.h"

namespace kernel {

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

}  // namespace

void DebugSessionService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointDebugAttach, &gDebugAttachHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugDetach, &gDebugDetachHandler);
}

}  // namespace kernel
