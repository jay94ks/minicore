#include "timerfd.h"

#include "async_task.h"
#include "delayed_exec.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "process.h"
#include "rtc.h"
#include "scheduler.h"  // kSchedulerTickHz - SP-A7479F83 §6-A 절대시각 환산용

namespace kernel {
namespace {

SharedPtr<Process> kProcessFromSubmitter(AsyncTask* task) {
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter || !submitter->isUserLevel) {
        return SharedPtr<Process>();
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return thread->process.lock();
}

constexpr int32_t kMaxFileDescriptorValue = 1024;
int32_t kAllocateFd(Process* process) {
    for (int32_t candidate = 0; candidate < kMaxFileDescriptorValue; ++candidate) {
        if (!process->fileDescriptors.find(
                [candidate](const Process::FileDescriptor& e) { return e.fd == candidate; })) {
            return candidate;
        }
    }
    return -1;
}

struct TimerfdCallbackContext {
    SharedPtr<TimerfdState> state;
};

void kFreeTimerfdCallbackContext(TimerfdCallbackContext* ctx) {
    ctx->~TimerfdCallbackContext();
    GenericSlabAllocator::free(ctx, sizeof(TimerfdCallbackContext));
}

// DelayedExecutionQueue::pump()가 AsyncReactor::drainOnce()의
// idle-fallback 분기 안에서 이 콜백을 부른다 - "지금 pump()를 처리
// 중인 코어"와 "대기 중인 Read 호출자의 homeCoreIndex"가 다를 수
// 있으므로(PN-96265AE4가 실측으로 확인), 대기자를 깨울 땐 반드시
// preemptive=true로 그 코어에 IPI를 보내 즉시 드레인을 강제한다
// (timerfd.h 문서 주석, PN-4FA5F13B가 확립한 원칙과 동일).
// [신규, 2026-09-29, SP-A7479F83 §6-C 답변] Close(fd)가 cancel()
// 실패(경쟁) 시 이 함수에게 최종 정리를 떠넘긴 경우 - fd 슬롯을 지금
// 대신 회수한다(timerfd.h TimerfdState::closing 문서 주석 참고).
void kReclaimClosingTimerfdSlot(TimerfdState* state) {
    SharedPtr<Process> process = state->ownerProcess.lock();
    if (!process) {
        return;
    }
    const int32_t fd = state->ownerFd;
    auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
    if (slot && slot->value.kind == MountKind::Timerfd && slot->value.timerfd.get() == state) {
        process->fileDescriptors.erase(slot);
    }
}

void kOnTimerfdFire(void* arg) {
    auto* ctx = static_cast<TimerfdCallbackContext*>(arg);
    TimerfdState* state = ctx->state.get();

    bool isClosing = false;
    AsyncTask* wake = nullptr;
    bool reschedule = false;
    uint64_t intervalTicks = 0;
    EpollObserverQueue wakeEpollObservers;
    {
        SpinlockGuard guard(state->lock);
        isClosing = state->closing;
        if (!isClosing) {
            state->expirationCount += 1;
            state->activeToken = 0;
            wake = state->pendingReaders.popFront();
            // [신규, 2026-09-29, SP-6350DEBB §5 통합] 레벨 트리거 재스캔
            // 신호일 뿐이라 전부 드레인한다(channel.cpp의 소켓 쓰기/읽기
            // 완료 시 readObservers/writeObservers를 전부 깨우는 것과
            // 동일한 관례).
            for (EpollObserverNode* n = state->epollReadObservers.popFront(); n;
                 n = state->epollReadObservers.popFront()) {
                wakeEpollObservers.pushBack(n);
            }
            if (state->periodic && state->intervalTicks > 0) {
                reschedule = true;
                intervalTicks = state->intervalTicks;
            }
        }
    }
    if (isClosing) {
        kReclaimClosingTimerfdSlot(state);
        kFreeTimerfdCallbackContext(ctx);
        return;
    }
    if (wake) {
        AsyncReactor::submitCompletion(wake, /*preemptive=*/true);
    }
    // [신규, 2026-09-29] pendingReaders 깨우기와 동일한 이유(timerfd.h
    // 문서 주석) - 이 콜백은 DelayedExecutionQueue::pump()의 idle-fallback
    // 경로에서 임의의 코어가 실행하므로, 관찰자의 homeCoreIndex가 다른
    // 코어면 preemptive=true로 즉시 IPI를 보내야 한다.
    for (EpollObserverNode* n = wakeEpollObservers.popFront(); n; n = wakeEpollObservers.popFront()) {
        AsyncReactor::submitCompletion(n->task, /*preemptive=*/true);
    }
    if (reschedule) {
        const uint64_t token = DelayedExecutionQueue::schedule(intervalTicks, &kOnTimerfdFire, ctx);
        if (token != 0) {
            SpinlockGuard guard(state->lock);
            state->activeToken = token;
            return;
        }
    }
    kFreeTimerfdCallbackContext(ctx);
}

class TimerfdCreateHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<TimerfdCreateArgs*>(argsRaw);
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        void* mem = GenericSlabAllocator::alloc(sizeof(TimerfdState));
        if (!mem) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        memset(mem, 0, sizeof(TimerfdState));
        auto* raw = reinterpret_cast<TimerfdState*>(mem);
        SharedPtr<TimerfdState> state = kMakeShared<TimerfdState>(raw);
        if (!state) {
            GenericSlabAllocator::free(mem, sizeof(TimerfdState));
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        state->periodic = args->periodic;

        process->fileDescriptors.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        const int32_t newFd = kAllocateFd(process.get());
        if (newFd < 0) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        // [신규, 2026-09-29, SP-A7479F83 §6-C 답변] Close(fd)/
        // kOnTimerfdFire가 경쟁 상황에서 이 fd 슬롯을 되짚어 지울 수
        // 있도록 소유자 정보를 미리 심어 둔다(timerfd.h 문서 주석 참고).
        state->ownerProcess = WeakPtr<Process>(process);
        state->ownerFd = newFd;
        Process::FileDescriptor fdEntry;
        fdEntry.fd = newFd;
        fdEntry.kind = MountKind::Timerfd;
        fdEntry.timerfd = state;
        fdEntry.used = true;
        if (!process->fileDescriptors.insert(fdEntry)) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        args->fd = newFd;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class TimerfdSetTimeHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<TimerfdSetTimeArgs*>(argsRaw);
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        const int32_t fd = args->fd;
        auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
        if (!slot || slot->value.kind != MountKind::Timerfd || !slot->value.timerfd) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        SharedPtr<TimerfdState> state = slot->value.timerfd;
        uint64_t oldToken = 0;
        {
            SpinlockGuard guard(state->lock);
            oldToken = state->activeToken;
            state->activeToken = 0;
            state->intervalTicks = state->periodic ? args->intervalTicks : 0;
        }
        if (oldToken != 0) {
            DelayedExecutionQueue::cancel(oldToken);
        }
        // [신규, 2026-09-29, SP-A7479F83 §6-A(절대시각 타이머)] absolute면
        // initialTicks를 목표 유닉스 타임스탬프(초)로 해석해, 설정
        // 시점에 Rtc를 딱 한 번 읽어 상대 틱으로 환산한다 - 그 뒤로는
        // 나머지 코드가 상대 틱 방식과 완전히 동일하게 스케줄한다.
        // 이미 지난 시각이면 0틱(다음 pump()에서 즉시 만료)으로
        // clamp한다 - 상대 모드의 0(=disarm, 아래 shouldSchedule)과
        // 달리 absolute는 항상 실제로 스케줄해야 한다(명시적으로 준
        // 목표 시각이 과거였을 뿐, "타이머를 걸지 말라"는 뜻이 아님).
        uint64_t effectiveInitialTicks = args->initialTicks;
        bool shouldSchedule = args->initialTicks > 0;
        if (args->absolute) {
            const WallClockTime now = Rtc::readWallClock();
            const uint64_t nowEpoch = Rtc::toEpochSeconds(now);
            effectiveInitialTicks =
                args->initialTicks > nowEpoch ? (args->initialTicks - nowEpoch) * kSchedulerTickHz : 0;
            shouldSchedule = true;
        }
        if (shouldSchedule) {
            void* mem = GenericSlabAllocator::alloc(sizeof(TimerfdCallbackContext));
            if (!mem) {
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }
            auto* ctx = new (mem) TimerfdCallbackContext();
            ctx->state = state;
            const uint64_t token = DelayedExecutionQueue::schedule(effectiveInitialTicks, &kOnTimerfdFire, ctx);
            if (token == 0) {
                kFreeTimerfdCallbackContext(ctx);
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }
            SpinlockGuard guard(state->lock);
            state->activeToken = token;
        }
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

TimerfdCreateHandler gTimerfdCreateHandler;
TimerfdSetTimeHandler gTimerfdSetTimeHandler;

}  // namespace

void Timerfd::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointTimerfdCreate, &gTimerfdCreateHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointTimerfdSetTime, &gTimerfdSetTimeHandler);
}

}  // namespace kernel
