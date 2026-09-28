#include "timerfd.h"

#include "async_task.h"
#include "delayed_exec.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "process.h"

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
void kOnTimerfdFire(void* arg) {
    auto* ctx = static_cast<TimerfdCallbackContext*>(arg);
    TimerfdState* state = ctx->state.get();

    AsyncTask* wake = nullptr;
    bool reschedule = false;
    uint64_t intervalTicks = 0;
    {
        SpinlockGuard guard(state->lock);
        state->expirationCount += 1;
        state->activeToken = 0;
        wake = state->pendingReaders.popFront();
        if (state->periodic && state->intervalTicks > 0) {
            reschedule = true;
            intervalTicks = state->intervalTicks;
        }
    }
    if (wake) {
        AsyncReactor::submitCompletion(wake, /*preemptive=*/true);
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
        if (args->initialTicks > 0) {
            void* mem = GenericSlabAllocator::alloc(sizeof(TimerfdCallbackContext));
            if (!mem) {
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }
            auto* ctx = new (mem) TimerfdCallbackContext();
            ctx->state = state;
            const uint64_t token = DelayedExecutionQueue::schedule(args->initialTicks, &kOnTimerfdFire, ctx);
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
