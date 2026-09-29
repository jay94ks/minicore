#include "signalfd.h"

#include "async_task.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "process.h"
#include "signal.h"

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

// [SP-0666DB3C §4.6, SP-A7479F83 §4] Kill/Stop은 어떤 경로로도
// 마스킹 불가 - SignalActionHandler(Ignore 거부)/SignalMaskHandler
// (Block/SetMask 거부)와 동일한 상수.
constexpr uint32_t kUnmaskableSignalBits = (1u << static_cast<uint32_t>(SignalNumber::Kill)) |
                                            (1u << static_cast<uint32_t>(SignalNumber::Stop));

// [SP-A7479F83 §4 핵심 결정] SignalfdSetMask가 지정한 시그널을
// Process::signalMask에도 동시에 블록한다 - syscall 왕복 없이
// Process::signalMask를 직접 조작하는 커널 내부 헬퍼(SP-0666DB3C §4.6
// 문서 주석이 이 재사용 형태를 그대로 예고해 뒀다). oldMask에서 빠지고
// newMask에만 있는 비트는 새로 Block, oldMask에 있었고 newMask에서
// 빠진 비트는 Unblock한다 - **알려진 v1 단순화**: 같은 시그널을 여러
// signalfd가 동시에 watch하면 한 fd의 SetMask가 다른 fd를 위해 걸어
//둔 블록까지 걷어낼 수 있다(RM-23F4B687 §4 수준의 구현 세부 - 여러
// signalfd 인스턴스 간 마스크 소유권 분리는 실사용 필요가 생기면 후속).
void kSyncProcessSignalMask(Process& process, uint32_t oldWatchedMask, uint32_t newWatchedMask) {
    const uint32_t toBlock = newWatchedMask & ~oldWatchedMask;
    const uint32_t toUnblock = oldWatchedMask & ~newWatchedMask;
    process.signalMask |= toBlock;
    process.signalMask &= ~toUnblock;
}

class SignalfdCreateHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SignalfdCreateArgs*>(argsRaw);
        if ((args->signalMask & kUnmaskableSignalBits) != 0) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        void* mem = GenericSlabAllocator::alloc(sizeof(SignalfdState));
        if (!mem) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        memset(mem, 0, sizeof(SignalfdState));
        auto* raw = reinterpret_cast<SignalfdState*>(mem);
        SharedPtr<SignalfdState> state = kMakeShared<SignalfdState>(raw);
        if (!state) {
            GenericSlabAllocator::free(mem, sizeof(SignalfdState));
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        state->watchedSignalMask = args->signalMask;

        process->fileDescriptors.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        const int32_t newFd = kAllocateFd(process.get());
        if (newFd < 0) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        state->ownerProcess = WeakPtr<Process>(process);
        state->ownerFd = newFd;
        Process::FileDescriptor fdEntry;
        fdEntry.fd = newFd;
        fdEntry.kind = MountKind::Signalfd;
        fdEntry.signalfd = state;
        fdEntry.used = true;
        if (!process->fileDescriptors.insert(fdEntry)) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        kSyncProcessSignalMask(*process, /*oldWatchedMask=*/0, args->signalMask);
        args->fd = newFd;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class SignalfdSetMaskHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SignalfdSetMaskArgs*>(argsRaw);
        if ((args->signalMask & kUnmaskableSignalBits) != 0) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        const int32_t fd = args->fd;
        auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
        if (!slot || slot->value.kind != MountKind::Signalfd || !slot->value.signalfd) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        SharedPtr<SignalfdState> state = slot->value.signalfd;
        uint32_t oldWatchedMask = 0;
        {
            SpinlockGuard guard(state->lock);
            oldWatchedMask = state->watchedSignalMask;
            state->watchedSignalMask = args->signalMask;
            state->pendingMask &= args->signalMask;  // 더 이상 관심 없는 신호는 펜딩에서도 제거
        }
        kSyncProcessSignalMask(*process, oldWatchedMask, args->signalMask);
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

SignalfdCreateHandler gSignalfdCreateHandler;
SignalfdSetMaskHandler gSignalfdSetMaskHandler;

}  // namespace

void Signalfd::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointSignalfdCreate, &gSignalfdCreateHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSignalfdSetMask, &gSignalfdSetMaskHandler);
}

}  // namespace kernel
