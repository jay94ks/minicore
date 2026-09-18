#include "debug_session.h"

#include "acpi.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "logger.h"
#include "paging.h"
#include "process.h"
#include "resource_group.h"
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

void kSaveDebugRegistersSnapshot(Task* task, InterruptFrame* frame) {
    if (!task->isUserLevel) {
        return;
    }
    auto* thread = static_cast<UserThread*>(task);
    SharedPtr<Process> proc = thread->process.lock();
    if (!proc || !proc->debugSession.active) {
        return;
    }
    // [갱신, 2026-09-19, PN-06A7C439] 스냅숏은 이제 `proc->debugSession`이
    // 아니라 이 스레드 자신(`thread`)에 찍는다 - 여러 스레드가 각자 다른
    // 순간에 정지해 들어올 수 있어(all-stop, `pausedByDebugger`는
    // 여전히 process-wide) 스냅숏 자체는 스레드별로 독립이어야 한다.
    DebugRegisterSnapshot& snap = thread->debugSavedRegisters;
    snap.rax = frame->rax;
    snap.rbx = frame->rbx;
    snap.rcx = frame->rcx;
    snap.rdx = frame->rdx;
    snap.rsi = frame->rsi;
    snap.rdi = frame->rdi;
    snap.rbp = frame->rbp;
    snap.r8 = frame->r8;
    snap.r9 = frame->r9;
    snap.r10 = frame->r10;
    snap.r11 = frame->r11;
    snap.r12 = frame->r12;
    snap.r13 = frame->r13;
    snap.r14 = frame->r14;
    snap.r15 = frame->r15;
    snap.rip = frame->rip;
    snap.cs = frame->cs;
    snap.rflags = frame->rflags;
    snap.rsp = frame->rspOld;
    snap.ss = frame->ssOld;
    thread->debugLiveFramePtr = frame;
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
        // [수정, 2026-09-18, PN-87D6B615 남은 범위 2번 착수 중 발견]
        // PN-C39882D0(ProcessId 세대 태그 슬롯 인코딩 마이그레이션)가
        // `SpawnProcessArgs::pid`/`WaitArgs::targetPid`를 raw
        // `reinterpret_cast<int64_t>(Process*)`에서 `Process::processId`
        // (kAllocateProcessId() 발급값)로 옮겼는데, 그 커밋은 명시적으로
        // "SpawnProcess/Wait만" 다룬다고 범위를 밝히며 Kill의 의도적
        // 과도기적 raw-pointer 비교는 별도로 언급했다(process.h
        // ProcessId 문서 주석) - 하지만 debug_session.cpp의 이 함수는
        // 그 목록 어디에도 언급되지 않은 채 예전 raw-pointer 비교
        // (`reinterpret_cast<int64_t>(child.get())`)로 그대로 남아
        // 있었다. 즉 실제 SpawnProcess 호출자가 반환받은(ProcessId
        // 인코딩) pid를 그대로 DebugAttach 등의 targetProcessId로
        // 넘기면 이 비교가 항상 실패해 PermissionDenied만 돌려줬다 -
        // Kill과 달리 이건 의도된 과도기적 예외가 아니라 단순히 그
        // 마이그레이션 커밋이 놓친 파일이었다(문서화된 예외 목록에
        // 없음). `child->processId`(SpawnProcess로 만들어진 자식만
        // kAllocateProcessId()로 채워짐 - 고정 스폰 KernelService는
        // 애초에 이 트리에 없어 이 비교 대상이 아님)로 바꿔 실제 pid
        // ABI와 맞춘다.
        if (child->processId == targetProcessId) {
            target = child;
        }
    });
    return target;
}

// [신규, 2026-09-19, SP-9A6D579F §1-A/§3.4/§3.5, PN-06A7C439]
// `DebugSetSingleStep`/`DebugGetRegisters`/`DebugSetRegisters`가
// targetThread로 지목한 스레드를 찾는다 - 없으면 nullptr(호출부가
// `NotFound`로 대응, 이 파일 전역 관례).
UserThread* kFindThreadById(SharedPtr<Process>& proc, ThreadId id) {
    UserThread* found = nullptr;
    proc->threads.forEach([&](SharedPtr<UserThread>& t, auto*) {
        if (!found && t && t->threadId == id) {
            found = t.get();
        }
    });
    return found;
}

// [갱신, 2026-09-19, PN-06A7C439] `DebugContinueHandler`가 하던 write-back
// 로직을 그대로 뽑아 옮긴 것뿐(동작 변화 없음) - 이제 `Process::
// debugSession`의 process당 하나뿐인 liveFramePtr/savedRegisters/
// singleStepPending이 아니라 이 `thread` 자신의 것을 쓴다는 점만 다르다
// (여러 스레드가 각자 정지해 있을 수 있어 DebugContinueHandler가 이
// 함수를 대상 프로세스의 정지된 스레드마다 반복 호출한다). 호출 전
// `thread->debugLiveFramePtr != nullptr`를 반드시 확인해야 한다.
void kWriteBackDebugFrame(UserThread* thread) {
    InterruptFrame* frame = thread->debugLiveFramePtr;
    const DebugRegisterSnapshot& snap = thread->debugSavedRegisters;
    frame->rax = snap.rax;
    frame->rbx = snap.rbx;
    frame->rcx = snap.rcx;
    frame->rdx = snap.rdx;
    frame->rsi = snap.rsi;
    frame->rdi = snap.rdi;
    frame->rbp = snap.rbp;
    frame->r8 = snap.r8;
    frame->r9 = snap.r9;
    frame->r10 = snap.r10;
    frame->r11 = snap.r11;
    frame->r12 = snap.r12;
    frame->r13 = snap.r13;
    frame->r14 = snap.r14;
    frame->r15 = snap.r15;
    frame->rip = snap.rip;
    frame->cs = snap.cs;
    // [구현 완료, 2026-09-17, SP-9A6D579F §3.4] RFLAGS.TF(비트
    // 8, 0x100)는 savedRegisters.rflags 사본을 그대로 되쓰지
    // 않고 singleStepPending에 따라 이 자리에서 명시적으로
    // 세우거나 지운다 - 정지 사유가 싱글스텝 트랩 자신이었을
    // 경우 snap.rflags에 TF=1이 이미 들어있어(트랩 시점의
    // 실제 EFLAGS를 그대로 스냅숏했으므로) 그걸 무비판적으로
    // 되쓰면 다음 명령에서 또 트랩해 무한 싱글스텝에 빠진다 -
    // DebugSetSingleStep을 다시 호출하지 않는 한 정상 실행으로
    // 돌아가야 하므로 매번 명시적으로 판단한다(syscall.h
    // debugSingleStepPending 문서 주석과 대칭).
    constexpr uint64_t kRflagsTrapFlag = 0x100;
    uint64_t rflags = snap.rflags & ~kRflagsTrapFlag;
    if (thread->debugSingleStepPending) {
        rflags |= kRflagsTrapFlag;
        thread->debugSingleStepPending = false;  // 한 번 쓰이면 소비됨
    }
    // [수정, 2026-09-18, PN-87D6B615 남은 범위 2번 실측 E2E 중
    // 발견] RFLAGS.RF(Resume Flag, 비트 16, 0x10000)를 세우지
    // 않으면, 정지 사유가 하드웨어 실행 브레이크포인트(B0-B3)
    // 였을 때 재개 직후 CPU가 같은 명령어를 다시 인출하며 그
    // 브레이크포인트 조건을 즉시 재검사해 또 트랩한다(Intel
    // SDM Vol.3 §17.3.1.1 - RF는 "IRETQ 직후 딱 한 명령어
    // 동안 명령어 브레이크포인트 재인식을 억제"하는 용도로
    // 정확히 이 상황을 위해 존재) - 그 결과 dbgtarget이
    // 실제로 한 걸음도 전진하지 못한 채 같은 RIP에서 영원히
    // 재정지하는 것을 실측으로 발견했다(devmgr+dbgtarget E2E
    // 하네스, PN-87D6B615). 싱글스텝(TF) 재개에는 원래
    // 영향이 없으므로(RF는 명령어 브레이크포인트 재인식만
    // 억제, TF 트랩 메커니즘과는 독립적) 정지 사유와 무관하게
    // 항상 세워도 안전하다.
    constexpr uint64_t kRflagsResumeFlag = 0x10000;
    rflags |= kRflagsResumeFlag;
    frame->rflags = rflags;
    frame->rspOld = snap.rsp;
    frame->ssOld = snap.ss;
    // 재사용/댕글링 방지 - 이 스레드가 다시 정지하기 전까지 무효.
    thread->debugLiveFramePtr = nullptr;
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

        // [수정, 2026-09-18, PN-87D6B615 남은 범위 2번 착수 중 발견]
        // 예전엔 여기서 `target->debugSession = DebugSession{};`로
        // 세션 전체를 통째로 새로 만들었는데, 이러면 `pausedByDebugger`
        // (+`savedRegisters`/`liveFramePtr`)까지 항상 초기화돼 버린다 -
        // 이 필드들은 "세션 설정"이 아니라 **디버기 자신의 실제 실행
        // 상태**(디버거가 붙어 있든 없든 참이어야 할 사실)라 세션을
        // 새로 여는 이 시점에 지워지면 안 된다. 구체적으로
        // `SpawnProcess`의 `kSpawnDebugStart`(process.cpp)가 자식을
        // 만들며 이미 `pausedByDebugger=true`로 세워 뒀는데, 여기서
        // 그걸 지우면 바로 다음에 오는 `DebugContinue`가
        // "`pausedByDebugger`가 아니다"로 오판해 `NotFound`를 돌려줘
        // 그 자식을 영원히 재개할 방법이 없어진다(이 세션이 실제
        // initrd E2E를 준비하며 코드 추적으로 발견 - 지금까지 이
        // 조합(kSpawnDebugStart + 진짜 DebugAttach)이 한 번도 실제로
        // 실행된 적이 없어 드러나지 않았던 버그). 세션 설정 필드만
        // 새로 초기화하고 실제 정지 상태(`pausedByDebugger`/
        // `savedRegisters`/`liveFramePtr`)는 그대로 둔다.
        target->debugSession.active = true;
        target->debugSession.debuggerProcess = WeakPtr<Process>(caller);
        for (auto& bp : target->debugSession.breakpoints) {
            bp = DebugBreakpoint{};
        }
        // [갱신, 2026-09-19, PN-06A7C439] `singleStepPending`은 이제
        // 스레드별(`UserThread::debugSingleStepPending`)이라 여기서도
        // 모든 스레드에 대해 초기화한다 - 위 breakpoints와 같은 "세션
        // 설정" 범주(요청 대기 플래그일 뿐 실제 실행 상태가 아님), 바로
        // 위 클래스 문서 주석이 `pausedByDebugger`/스레드별 스냅숏은
        // 그대로 둬야 한다고 구분해 둔 것과 대칭.
        target->threads.forEach([](SharedPtr<UserThread>& t, auto*) {
            if (t) {
                t->debugSingleStepPending = false;
            }
        });
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
        // 준다. 실제 하드웨어 반영은 대상(target->threads의 스레드)이
        // 다음 디스패치될 때 `Scheduler::onTick()` 등이 부르는
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

// [구현 완료, 2026-09-17, SP-9A6D579F §3.4, PN-87D6B615 "남은 범위"
// 1번] DebugSetSingleStep - DebugSetBreakpointHandler와 동일한 권한
// 검증에 더해, 대상이 지금 정지 상태(`pausedByDebugger`)여야 한다
// (GetRegisters/SetRegisters와 동일한 이유 - 정지 상태가 아니면 다음
// DebugContinue 자체가 존재하지 않아 이 플래그를 세워도 적용될 자리가
// 없다). 하드웨어는 전혀 건드리지 않는다 - `singleStepPending`만
// 세우거나 내리고, 실제 RFLAGS.TF 반영은 DebugContinue의 write-back
// 몫이다(debug_session.h `singleStepPending` 문서 주석 참고).
class DebugSetSingleStepHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DebugSetSingleStepArgs*>(argsRaw);

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
        // [갱신, 2026-09-19, PN-06A7C439] process-wide `pausedByDebugger`
        // 대신 targetThread 자신이 실제로 유효한 정지 스냅숏을 갖고
        // 있는지(`debugLiveFramePtr != nullptr`)로 검증한다 - 여러
        // 스레드가 동시에 정지해 있을 수 있어 "이 특정 스레드"가 정지
        // 상태인지를 정확히 가려야 한다.
        UserThread* thread = kFindThreadById(target, args->targetThread);
        if (!thread || !thread->debugLiveFramePtr) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        thread->debugSingleStepPending = args->enable;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

DebugSetSingleStepHandler gDebugSetSingleStepHandler;

// [신규, 2026-09-17, SP-9A6D579F §3.5, PN-87D6B615 항목5] DebugContinue -
// 정지된 대상을 Blocked에서 다시 Ready로. 레지스터 상태를 전혀
// 건드리지 않으므로(그건 GetRegisters/SetRegisters의 몫이자 아직
// 미해결 설계 자리, debug_session.h 상단 주석 참고) DebugSetBreakpoint
// 와 동일한 권한 검증만 거치면 구현 가능하다.
class DebugContinueHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DebugContinueArgs*>(argsRaw);

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
        if (!target->debugSession.pausedByDebugger) {
            // 정지된 적이 없거나 이미 재개됨 - InvalidState가 없는 이
            // 코드베이스 관례대로 NotFound로 대체(§6/DebugSetBreakpoint
            // 문서 주석과 동일한 이유).
            args->error = ChannelError::NotFound;
            co_return;
        }

        // [갱신, 2026-09-19, PN-06A7C439] process당 하나였던 liveFramePtr
        // 대신, 이 프로세스의 스레드 중 지금 실제로 정지 스냅숏을 갖고
        // 있는 스레드 전부(`debugLiveFramePtr != nullptr`)를 순회하며
        // 각자 자기 것으로 write-back한다 - all-stop 시맨틱이라 여러
        // 스레드가 동시에 정지해 있을 수 있고, `DebugContinue`는
        // 그 전부를 한 번에 재개한다(§3.5 write-back 자체의 이유는
        // 변화 없음 - kWriteBackDebugFrame 문서 주석 참고).
        target->threads.forEach([](SharedPtr<UserThread>& threadRef, auto*) {
            if (UserThread* t = threadRef.get()) {
                if (t->debugLiveFramePtr) {
                    kWriteBackDebugFrame(t);
                }
            }
        });

        // [SP-245D130B §9-4 교차 기록, ResourceGroup::thaw()와 대칭]
        // pausedByDebugger는 항상 내려놓지만(디버거가 정지를 풀기로
        // 결정했으므로), 그룹이 아직 frozen이면 실제로 깨우지 않는다 -
        // frozenByGroup이 이미 서 있어(kCheckAndMarkFrozen) 나중에
        // ResourceGroup::thaw()가 대신 깨운다.
        target->debugSession.pausedByDebugger = false;
        // [갱신, 2026-09-19, PN-06A7C439] `target->threads` 전체를
        // 무조건 재개한다 - `DebugContinueArgs` 문서 주석대로 의도적인
        // all-stop→continue-all 시맨틱(선택적으로 스레드 하나만 재개하는
        // 기능은 이 계획 범위 밖, 필요해지면 별도 계획으로 targetThread를
        // 추가한다).
        if (!(target->group && target->group->frozen)) {
            target->threads.forEach([](SharedPtr<UserThread>& threadRef, auto*) {
                if (UserThread* t = threadRef.get()) {
                    Scheduler::enqueue(Scheduler::currentCoreIndex(), t);
                }
            });
        }
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

DebugContinueHandler gDebugContinueHandler;

// [갱신, 2026-09-19, PN-06A7C439] DebugGetRegisters - targetThread로
// 지목한 그 스레드가 정지 상태여야 하고(그래야 그 스레드 자신의
// debugSavedRegisters/debugLiveFramePtr가 유효), 그 외 권한 검증은
// DebugSetBreakpoint와 동일한 패턴.
class DebugGetRegistersHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DebugGetRegistersArgs*>(argsRaw);

        if (!Paging::isUserRangeValid(reinterpret_cast<uint64_t>(args->out), sizeof(DebugRegisterSnapshot))) {
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
        UserThread* thread = kFindThreadById(target, args->targetThread);
        if (!thread || !thread->debugLiveFramePtr) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        *args->out = thread->debugSavedRegisters;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

DebugGetRegistersHandler gDebugGetRegistersHandler;

// [갱신, 2026-09-19, PN-06A7C439] DebugSetRegisters - DebugGetRegistersHandler
// 와 대칭(방향만 반대, targetThread 포함). 이 호출 자체는 재개하지
// 않는다 - 그 스레드의 사본(debugSavedRegisters)만 갱신, 실제 반영은
// DebugContinue가 write-back할 때(syscall.h 상단 주석 참고).
class DebugSetRegistersHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DebugSetRegistersArgs*>(argsRaw);

        if (!Paging::isUserRangeValid(reinterpret_cast<uint64_t>(args->in), sizeof(DebugRegisterSnapshot))) {
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
        UserThread* thread = kFindThreadById(target, args->targetThread);
        if (!thread || !thread->debugLiveFramePtr) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        thread->debugSavedRegisters = *args->in;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

DebugSetRegistersHandler gDebugSetRegistersHandler;

// [신규, 2026-09-17, SP-9A6D579F §3.5, PN-87D6B615 항목6] 디버기의
// [pageBase, pageBase+4096) 구간 하나를 Paging::translatePage()로
// 직접 물리 프레임을 찾아 direct map 경유로 읽거나 쓴다(대행 복사 -
// 디버기 주소공간을 디버거 쪽에 매핑하지 않음, §3.5 그대로) - 여러
// 페이지에 걸친 [addr, addr+length) 구간은 호출부가 페이지 경계마다
// 나눠 반복 호출한다. 매핑 안 된 페이지를 만나면 false(이미 복사된
// 앞부분은 되돌리지 않음 - 호출부가 어차피 에러로 실패 보고).
bool kCopyDebuggeeMemory(uint64_t targetPml4Phys, uint64_t addr, uint64_t length, uint8_t* kernelBuf,
                          bool fromDebuggee) {
    uint64_t remaining = length;
    uint64_t cur = addr;
    uint8_t* buf = kernelBuf;
    while (remaining > 0) {
        const uint64_t pageBase = cur & ~0xFFFULL;
        const uint64_t pageOffset = cur - pageBase;
        uint64_t chunk = 4096ULL - pageOffset;
        if (chunk > remaining) {
            chunk = remaining;
        }
        const uint64_t phys = Paging::translatePage(pageBase, targetPml4Phys);
        if (!phys) {
            return false;
        }
        uint8_t* kernelPagePtr = reinterpret_cast<uint8_t*>(kPhysToVirt(phys)) + pageOffset;
        if (fromDebuggee) {
            memcpy(buf, kernelPagePtr, chunk);
        } else {
            memcpy(kernelPagePtr, buf, chunk);
        }
        buf += chunk;
        cur += chunk;
        remaining -= chunk;
    }
    return true;
}

// [신규, 2026-09-17, SP-9A6D579F §3.5, PN-87D6B615 항목6] DebugReadMemory.
class DebugReadMemoryHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DebugReadMemoryArgs*>(argsRaw);

        // 디버거 자신의 출력 버퍼 검증 - 기본 pml4Phys(현재 CR3)가
        // 곧 호출자 자신의 주소공간이라는 이 코드베이스 전역 관례
        // (paging.h의 isUserRangeValid 문서 주석 그대로).
        if (!Paging::isUserRangeValid(reinterpret_cast<uint64_t>(args->out), args->length)) {
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

        if (args->length > 0) {
            if (!Paging::isUserRangeValid(args->address, args->length, target->pml4Phys)) {
                args->error = ChannelError::InvalidArgument;
                co_return;
            }
            if (!kCopyDebuggeeMemory(target->pml4Phys, args->address, args->length,
                                      static_cast<uint8_t*>(args->out), /*fromDebuggee=*/true)) {
                args->error = ChannelError::InvalidArgument;
                co_return;
            }
        }
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

DebugReadMemoryHandler gDebugReadMemoryHandler;

// [신규, 2026-09-17, SP-9A6D579F §3.5, PN-87D6B615 항목6] DebugWriteMemory -
// DebugReadMemoryHandler와 완전히 대칭(방향만 반대).
class DebugWriteMemoryHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DebugWriteMemoryArgs*>(argsRaw);

        if (!Paging::isUserRangeValid(reinterpret_cast<uint64_t>(args->in), args->length)) {
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

        if (args->length > 0) {
            if (!Paging::isUserRangeValid(args->address, args->length, target->pml4Phys)) {
                args->error = ChannelError::InvalidArgument;
                co_return;
            }
            if (!kCopyDebuggeeMemory(target->pml4Phys, args->address, args->length,
                                      const_cast<uint8_t*>(static_cast<const uint8_t*>(args->in)),
                                      /*fromDebuggee=*/false)) {
                args->error = ChannelError::InvalidArgument;
                co_return;
            }
        }
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

DebugWriteMemoryHandler gDebugWriteMemoryHandler;

// [신규, 2026-09-18, PN-49C2F890, 설계자 지시("디버그 인터럽트에서
// 해당 task를 블록 처리하고 블로킹 원인으로 디버거를 셋팅해두면
// 이런 복잡한 메커니즘이 필요하지 않게된다")] 코어별 "지금 이
// 코어의 #DB 전용 IST4 위에 kHandleUserBreakpointHit()의 콜 체인이
// Scheduler::parkCurrent()로 얼어붙어 있다" 플래그 - #DB가 발생할
// 때마다 CPU가 그 코어의 TSS.ist4를 같은 고정 최상단 주소로 리셋하는
// 공유 스택(gdt.cpp의 gIstStacks)이라, 이 플래그가 서 있는 동안 같은
// 코어에서 또 다른 #DB가 파킹을 시도하면 첫 번째 Task의 얼어붙은
// 프레임이 덮어써져 손상된다(QU-8172431E에서 이 세션이 확인한 위험).
// 설계자가 QU-8172431E에서 세 후보 중 "코어당 동시 파킹 1개 제한"
// 정책을 확정 - 이 배열이 그 정책의 유일한 강제 지점이다. 두 번째
// 이후의 히트는 안전하게 기존 지연 경로(pausedByDebugger만 세우고
// 반환 - Scheduler::onTick()이 다음 틱에 처리)로 대체하면 되는데,
// 이 시점엔 이미 첫 번째로 파킹된 Task가 이 코어에 "대체 실행
// 후보"로 존재하므로 PN-49C2F890 원래 갭(대체 후보 자체가 없어
// 영원히 검사가 안 도는 문제)이 애초에 성립하지 않는다.
bool gDebugParkedOnCore[kAcpiMaxCpus] = {};

// [갱신, 2026-09-18, PN-49C2F890] #DB ISR(idt.cpp의
// kHandleDebugException)이 하드웨어 브레이크포인트 적중 시 부르는
// 콜백 - Idt::registerDebugCallback()으로 등록한다.
//
// 이 코어에 아직 다른 디버깅 대상이 파킹돼 있지 않으면(위
// gDebugParkedOnCore), `Scheduler::parkCurrent()`를 직접 호출해
// **그 자리에서 즉시** Task를 Blocked로 전환하고 idle로 넘긴다 -
// `parkCurrent()`는 이 함수(`kHandleDebugException`←`kIsrHandler`
// ←`isr_common_stub`의 C 호출 체인 안에 있음)의 콜리세이브
// 레지스터를 IST4 스택 위에 남겨 둔 채 떠났다가, 나중에 누군가
// `Scheduler::enqueue()`로 다시 큐에 넣어 정상 재개되면 바로 이
// 호출 다음 지점부터 이어서 실행된다 - 그대로 반환하면
// `isr_common_epilogue`의 pop+iretq를 거쳐 "브레이크포인트가 걸렸던
// 바로 그 ring3 지점"으로 정확히 복귀한다(원리상 PN-44C91D6E가
// `kResumeForkedRing3`을 검증하며 확인한 것과 같은 "인터럽트 프레임
// 그대로 재사용" 패턴). 이미 다른 대상이 파킹돼 있으면(코어당 1개
// 제한, 위 플래그 문서 참고) 기존 지연 경로로 안전하게 대체한다.
bool kHandleUserBreakpointHit(InterruptFrame* frame, uint64_t dr6) {
    constexpr uint64_t kDr6BreakpointMask = 0xF;      // B0-B3(하드웨어 브레이크포인트)
    // [구현 완료, 2026-09-17, SP-9A6D579F §3.4, PN-87D6B615 "남은
    // 범위" 1번] BS(비트 14) - RFLAGS.TF로 유발된 싱글스텝 트랩. 이
    // 콜백이 실제로 "왜 멈췄는지"를 구분할 필요는 없다(하드웨어
    // 브레이크포인트든 싱글스텝이든 아래 로직은 완전히 동일 -
    // pausedByDebugger를 세우고 true 반환) - 그래서 두 마스크를 OR로
    // 합쳐 하나의 조건으로 취급한다.
    constexpr uint64_t kDr6SingleStepMask = 0x4000;   // BS
    if ((dr6 & (kDr6BreakpointMask | kDr6SingleStepMask)) == 0) {
        // 이 두 비트 다 없는 다른 DR6 상태(예: 태스크 스위치 트랩류,
        // 이 커널에서 쓰지 않음) - false를 반환해 idt.cpp가 기존처럼
        // 로그만 남기고 계속 실행하게 둔다.
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

    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    if (gDebugParkedOnCore[coreIndex]) {
        // [수정, 2026-09-19, PN-06A7C439 실측 발견 - 진짜 멀티스레드
        // 동시 히트로 처음 노출된 잠재 버그] 이미 이 코어에서 다른
        // 디버깅 대상이 파킹돼 있다 - 위 정책상 이번엔 파킹하지 않고
        // 기존 지연 경로(다음 스케줄러 틱의 kIsPausedByDebugger 검사,
        // scheduler.cpp)로 대체한다. **여기서 kSaveDebugRegistersSnapshot()
        // 를 부르면 안 된다** - `frame`은 이 코어의 공유 IST4 트랩
        // 프레임(고정 최상단 리셋 주소)인데, 이미 다른 스레드가 바로
        // 그 자리에 `parkCurrent()`로 얼어붙어 있는 중이라(스레드
        // 하나뿐이던 시절엔 같은 코어에서 진짜 서로 다른 스레드가 동시에
        // #DB를 두 번 낼 수 없어 드러나지 않았던 gap) 여기서 스냅숏을
        // 찍으면 그 얼어붙은 프레임 메모리를 이 스레드 것으로 덮어써
        // 버린다 - 실측 재현: dbgtarget 2-스레드 하네스(PN-06A7C439)에서
        // 이렇게 얻은 `debugLiveFramePtr`로 두 스레드 모두 write-back한
        // 뒤 재개하니 한쪽이 커널 주소로 rip가 튀어 Invalid Opcode로
        // PANIC(스택 내용이 실제로 덮어써졌다는 증거). 이 스레드 자신의
        // 진짜 스냅숏은 나중에 "지연 경로"(onTick()이 이 스레드 자신의
        // 전용 커널 스택 위 프레임으로 안전하게 찍음)에서만 채워지게
        // 그냥 둔다.
        return true;
    }
    gDebugParkedOnCore[coreIndex] = true;
    // [신규, 2026-09-18, PN-49C2F890] 이 지점부터는 이 코어의 IST4가
    // 진짜로 이 스레드 전용으로 얼어붙으므로(gDebugParkedOnCore 가드가
    // 보장) `frame`을 안전하게 스냅숏 대상으로 쓸 수 있다 - 위 분기와
    // 반대로 여기서만 찍는다(2026-09-18, PN-06A7C439로 위치 이동 -
    // 원래는 이 가드 확인 전에 무조건 찍고 있었다).
    //
    // [알려진 갭, 2026-09-19, PN-06A7C439 실측 발견 - 미해결] 위
    // gDebugParkedOnCore 재확인이 "같은 코어에서 동시에 두 스레드가
    // 파킹 시도"만 막을 뿐, **이 스레드가 여기서 parkCurrent()로 얼어붙어
    // 있는 동안, 같은 코어에서 다른(정지되지 않은) 형제 스레드가 정상
    // 실행되다가 이 프로세스의 공유 브레이크포인트를 다시 히트하거나
    // 타이머 틱으로 지연 경로(onTick())를 타는 상호작용까지는 막지
    // 못한다** - 실제 dbgtarget 2-스레드 하네스(같은 프로세스, 공유
    // EXECUTE 브레이크포인트)로 재현: 그런 상호작용이 겹치면 이미 정지된
    // 스레드의 `debugSavedRegisters`가 손상되고(rip가 세그먼트 셀렉터
    // 값처럼 보이는 임의 값으로 바뀌거나 커널 주소로 튐) 재개 시 Invalid
    // Opcode/Page Fault로 PANIC한다 - 근본 원인은 아직 확정하지 못했다
    // (IST4 자체의 재사용은 아닌 것으로 보임 - gIstStacks는 코어별로
    // 이미 분리돼 있음, PN-EA968DF0 참고). `Process::debugSession`/
    // `UserThread`의 디버그 필드들이 AsyncReactor(BSP 전용, 직렬화 보장)
    // 를 거치지 않고 이 함수와 `Scheduler::onTick()`처럼 **원시 ISR/
    // 스케줄러 틱 컨텍스트에서 여러 코어가 직접 동시에** 건드린다는 점이
    // 유력한 용의선 - 별도 세션의 집중 조사가 필요(PN-EA968DF0).
    kSaveDebugRegistersSnapshot(task, frame);
    Scheduler::parkCurrent();  // 재개될 때까지(DebugContinue 등) 여기서 멈춘다
    gDebugParkedOnCore[coreIndex] = false;
    return true;
}

}  // namespace

void DebugSessionService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointDebugAttach, &gDebugAttachHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugDetach, &gDebugDetachHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugSetBreakpoint, &gDebugSetBreakpointHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugSetSingleStep, &gDebugSetSingleStepHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugContinue, &gDebugContinueHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugGetRegisters, &gDebugGetRegistersHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugSetRegisters, &gDebugSetRegistersHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugReadMemory, &gDebugReadMemoryHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDebugWriteMemory, &gDebugWriteMemoryHandler);
}

void DebugSessionService::registerDebugCallback() {
    Idt::registerDebugCallback(&kHandleUserBreakpointHit);
}

}  // namespace kernel
