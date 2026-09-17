#include "vfs_syscall.h"

#include "libkenv/spinlock.h"
#include "mount_table.h"
#include "paging.h"
#include "process.h"
#include "task.h"

namespace kernel {

namespace {

// [channel.cpp의 kValidateUserBuffer(PN-B552E75F)/pnp.cpp의
// kValidateEnumerateBuffer와 동일한 패턴 재사용 - 관계도에 기록해 둠]
// path가 호출자 자신의 유저 주소공간에 속하는지 제출자(submitterTask)
// 의 실제 userPml4Phys로 검증한다 - onExec()이 AsyncReactor 컨텍스트
// 에서 나중에 실행되므로 "현재 CR3" 기본값을 믿을 수 없다는 동일한
// 이유(async_task.h의 submitterTask 주석 참고).
bool kValidateVfsBuffer(AsyncTask* task, const void* ptr, uint64_t length) {
    if (length == 0) {
        return false;  // Mount/Unmount/ResolvePath 전부 path 없이는 무의미(길이 0은 항상 거부)
    }
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter) {
        return false;
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return Paging::isUserRangeValid(reinterpret_cast<uint64_t>(ptr), length, thread->userPml4Phys);
}

class MountHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<MountArgs*>(argsRaw);
        if (!kValidateVfsBuffer(task, args->path, args->pathLen)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        args->error =
            MountTable::mount(args->path, args->pathLen, args->channelId) ? ChannelError::None : ChannelError::AlreadyExists;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

MountHandler gMountHandler;

class UnmountHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<UnmountArgs*>(argsRaw);
        if (!kValidateVfsBuffer(task, args->path, args->pathLen)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        args->error = MountTable::unmount(args->path, args->pathLen) ? ChannelError::None : ChannelError::NotFound;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

UnmountHandler gUnmountHandler;

class ResolvePathHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ResolvePathArgs*>(argsRaw);
        if (!kValidateVfsBuffer(task, args->path, args->pathLen)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        MountKind kind{};
        uint64_t channelId = 0;
        KernelFsDriver* kernelDriver = nullptr;
        uint32_t relOffset = 0;
        if (!MountTable::resolve(args->path, args->pathLen, &kind, &channelId, &kernelDriver, &relOffset)) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        if (kind == MountKind::KernelDriver) {
            // vfs_syscall.h의 ResolvePathArgs 문서 주석 참고 - 아직
            // 확정 안 된 영역, 임의로 채우지 않는다(CLAUDE.md 규칙 4).
            args->error = ChannelError::NotSupported;
            co_return;
        }
        args->ownerChannelId = channelId;
        args->relPathOffset = relOffset;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

ResolvePathHandler gResolvePathHandler;

// [SP-7CC5693A §2.5] "커널 전역으로 단 1회만 호출 가능" - CAS로 0→1
// 전이를 딱 한 번만 허용한다.
AtomicU32 gUserlandReady{0};

class SignalUserlandReadyHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<SignalUserlandReadyArgs*>(argsRaw);
        uint32_t expected = 0;
        args->error = gUserlandReady.compareExchange(expected, 1) ? ChannelError::None : ChannelError::AlreadyExists;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

SignalUserlandReadyHandler gSignalUserlandReadyHandler;

class WaitForUserlandReadyHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<WaitForUserlandReadyArgs*>(argsRaw);
        while (gUserlandReady.load() == 0) {
            AsyncTask::yield();
        }
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    // [검토 완료] SubscribeInterrupt류와 달리 이 대기는 어떤 침습적
    // 대기 큐에도 자신을 걸지 않는다(순수 폴링) - 취소돼도 정리할
    // 자원이 없다(PN-C4611402/RM-F2DAFF66 §1-B/E가 다룬 결함 클래스와
    // 무관 - 이 핸들러는 그 패턴 자체를 안 씀).
    void onCancel(AsyncTask*, void*) override {}
};

WaitForUserlandReadyHandler gWaitForUserlandReadyHandler;

}  // namespace

void VfsSyscallService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointMount, &gMountHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointUnmount, &gUnmountHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointResolvePath, &gResolvePathHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSignalUserlandReady, &gSignalUserlandReadyHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointWaitForUserlandReady, &gWaitForUserlandReadyHandler);
}

}  // namespace kernel
