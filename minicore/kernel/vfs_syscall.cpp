#include "vfs_syscall.h"

#include "libkenv/spinlock.h"
#include "libkmm/slab.h"
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

// [channel.cpp의 kProcessFromSubmitter/pnp.cpp의
// kProcessFromSubmitterForPnp와 동일한 패턴 재사용] AsyncTask 제출자의
// Process를 얻는다 - 제출자가 이미 죽었거나 Process가 종료됐으면 빈
// SharedPtr.
SharedPtr<Process> kProcessFromSubmitter(AsyncTask* task) {
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter) {
        return SharedPtr<Process>();
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return thread->process.lock();
}

// VfsError(mount_table.h, KernelFsDriver 전용 - §9 이전부터 존재하던
// 잠정 타입)를 ChannelError(§9.3 syscall ABI가 실제로 쓰는 타입)로
// 매핑한다 - 두 enum 다 이 프로젝트 관례상 값 재사용이 원칙이라 새
// ChannelError 값을 추가하지 않는다.
ChannelError kMapVfsError(VfsError error) {
    switch (error) {
        case VfsError::None:
            return ChannelError::None;
        case VfsError::NotFound:
            return ChannelError::NotFound;
        case VfsError::InvalidHandle:
            return ChannelError::InvalidHandle;
        case VfsError::PermissionDenied:
            return ChannelError::PermissionDenied;
        case VfsError::InvalidArgument:
            return ChannelError::InvalidArgument;
    }
    return ChannelError::InvalidArgument;
}

// [신규, PN-EA4EE935] 이 프로세스에서 아직 안 쓰는 가장 작은 fd를
// 고른다(POSIX "가장 낮은 번호 재사용" 관례) - v1은 프로세스당 fd
// 개수가 극히 적을 것으로 예상돼(ChunkedList 청크 용량 8) 단순 선형
// 탐색으로 충분하다(실측 후 조정, RM-23F4B687 §4).
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

// [SP-2AAD7C8D §9.3, PN-EA4EE935] Open/Close/Read/Write - vfs_syscall.h
// 상단 주석 참고: MountKind::Channel 마운트는 아직 NotSupported로만
// 응답한다(와이어 포맷 미정, CLAUDE.md 규칙 4). KernelDriver 마운트는
// AsyncTask::submit(driver->subjectCode(), ...) + AsyncTaskAwaiter로
// 실제 호출한다 - 이 프로젝트에서 KernelFsDriver를 처음으로 실제
// 소비하는 코드다(AsyncTaskAwaiter 자신도 이전까지 실사용처가 없었음).
class OpenHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<OpenArgs*>(argsRaw);
        if (!kValidateVfsBuffer(task, args->path, args->pathLen)) {
            args->fd = -1;
            args->error = ChannelError::InvalidPointer;
            co_return;
        }

        MountKind kind{};
        uint64_t channelId = 0;
        KernelFsDriver* driver = nullptr;
        uint32_t relOffset = 0;
        if (!MountTable::resolve(args->path, args->pathLen, &kind, &channelId, &driver, &relOffset)) {
            args->fd = -1;
            args->error = ChannelError::NotFound;
            co_return;
        }
        if (kind == MountKind::Channel) {
            // [정직하게 기록, PN-EA4EE935 스코프 결정] §9.1의 "그
            // Channel로 Open IPC 메시지 전송"의 실제 와이어 포맷이
            // 어디에도 정의돼 있지 않다 - 임의로 만들지 않는다.
            args->fd = -1;
            args->error = ChannelError::NotSupported;
            co_return;
        }

        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->fd = -1;
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }

        KernelFsOpenArgs kfsArgs;
        kfsArgs.relPath = args->path + relOffset;
        kfsArgs.relPathLen = args->pathLen - relOffset;
        kfsArgs.flags = args->flags;
        AsyncTask* fsTask = AsyncTask::submit(driver->subjectCode(), 0, &kfsArgs, /*autoFree=*/false);
        if (!fsTask) {
            args->fd = -1;
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        // [실측으로 발견, PN-EA4EE935] async_task.h의 submitterTask 문서
        // 주석이 이미 예견해 둔 "내부 재제출은 이 필드를 안 채운다 -
        // 실사용처가 생기면 확장"이 바로 이 지점이다 - ProcFs::open()의
        // "proc/self" 해석처럼 KernelFsDriver 구현체가 원 호출자 신원을
        // 알아야 하는 경우, 이 propagate 없이는 항상 빈 WeakPtr을 보고
        // PermissionDenied로 실패한다(실측 확인).
        fsTask->submitterTask = task->submitterTask;
        AsyncTaskAwaiter(fsTask).await();

        if (kfsArgs.result.error != VfsError::None) {
            args->fd = -1;
            args->error = kMapVfsError(kfsArgs.result.error);
            co_return;
        }

        process->fileDescriptors.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        int32_t newFd = kAllocateFd(process.get());
        if (newFd < 0) {
            // [정직하게 기록] 이미 열어 둔 kfsArgs.result.handle을 여기서
            // Close로 되돌리지 않는다 - fd 테이블이 가득 찬 극히 드문
            // 경우(1024개)라 실무 영향이 없고, 되돌리려면 이 실패
            // 경로에서 또 다른 AsyncTask 왕복을 넣어야 해 오히려 복잡도만
            // 늘어난다(실제로 이 경로를 밟을 상황이 생기면 재검토).
            args->fd = -1;
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }

        Process::FileDescriptor fdEntry;
        fdEntry.fd = newFd;
        fdEntry.kind = MountKind::KernelDriver;
        fdEntry.kernelDriver = driver;
        fdEntry.fsHandle = kfsArgs.result.handle;
        fdEntry.offset = 0;
        fdEntry.isDirectory = kfsArgs.result.isDirectory;
        fdEntry.used = true;
        process->fileDescriptors.insert(fdEntry);

        args->fd = newFd;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

OpenHandler gOpenHandler;

class CloseHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<CloseArgs*>(argsRaw);
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        const int32_t fd = args->fd;
        auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
        if (!slot) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        if (slot->value.kind == MountKind::KernelDriver) {
            KernelFsCloseArgs kfsArgs;
            kfsArgs.handle = slot->value.fsHandle;
            // KernelFsCloseArgs는 out 필드가 없다(mount_table.h 원안 그대로) -
            // 실패를 보고할 방법 자체가 설계에 없어 결과를 확인하지 않는다.
            AsyncTask* fsTask = AsyncTask::submit(slot->value.kernelDriver->subjectCode(), 0, &kfsArgs,
                                                   /*autoFree=*/false);
            if (fsTask) {
                fsTask->submitterTask = task->submitterTask;  // 실측 발견, PN-EA4EE935 - OpenHandler 주석 참고
                AsyncTaskAwaiter(fsTask).await();
            }
        }
        process->fileDescriptors.erase(slot);
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

CloseHandler gCloseHandler;

class ReadHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ReadArgs*>(argsRaw);
        // [정직하게 기록] kValidateVfsBuffer는 length==0을 항상 거부한다
        // (Mount류의 "빈 경로는 무의미" 전제로 설계됨) - POSIX의
        // `read(fd, buf, 0)`(즉시 0 반환) 관례와는 안 맞지만, 이 프로젝트
        // 어디에도 아직 0바이트 읽기 소비자가 없어 이번 증분은 기존
        // 헬퍼를 그대로 재사용한다(len==0 요청은 InvalidPointer로 거부됨 -
        // 실제 필요해지면 별도 처리 추가).
        if (!kValidateVfsBuffer(task, args->buf, args->len)) {
            args->bytesRead = 0;
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->bytesRead = 0;
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        const int32_t fd = args->fd;
        auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
        if (!slot) {
            args->bytesRead = 0;
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        if (slot->value.kind != MountKind::KernelDriver) {
            args->bytesRead = 0;
            args->error = ChannelError::NotSupported;  // Channel 경로 - PN-EA4EE935 스코프 밖
            co_return;
        }

        KernelFsReadArgs kfsArgs;
        kfsArgs.handle = slot->value.fsHandle;
        kfsArgs.offset = slot->value.offset;
        kfsArgs.buf = args->buf;
        kfsArgs.len = args->len;
        AsyncTask* fsTask = AsyncTask::submit(slot->value.kernelDriver->subjectCode(), 0, &kfsArgs,
                                               /*autoFree=*/false);
        if (!fsTask) {
            args->bytesRead = 0;
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        fsTask->submitterTask = task->submitterTask;  // 실측 발견, PN-EA4EE935 - OpenHandler 주석 참고
        AsyncTaskAwaiter(fsTask).await();

        if (kfsArgs.result.error != VfsError::None) {
            args->bytesRead = 0;
            args->error = kMapVfsError(kfsArgs.result.error);
            co_return;
        }
        slot->value.offset += kfsArgs.result.bytesRead;  // §9.2 - 커널(fd 테이블)이 커서를 소유
        args->bytesRead = kfsArgs.result.bytesRead;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

ReadHandler gReadHandler;

class WriteHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<WriteArgs*>(argsRaw);
        // [정직하게 기록] 위 ReadHandler와 동일한 len==0 한계.
        if (!kValidateVfsBuffer(task, args->buf, args->len)) {
            args->bytesWritten = 0;
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->bytesWritten = 0;
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        const int32_t fd = args->fd;
        auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
        if (!slot) {
            args->bytesWritten = 0;
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        if (slot->value.kind != MountKind::KernelDriver) {
            args->bytesWritten = 0;
            args->error = ChannelError::NotSupported;  // Channel 경로 - PN-EA4EE935 스코프 밖
            co_return;
        }

        KernelFsWriteArgs kfsArgs;
        kfsArgs.handle = slot->value.fsHandle;
        kfsArgs.offset = slot->value.offset;
        kfsArgs.buf = args->buf;
        kfsArgs.len = args->len;
        AsyncTask* fsTask = AsyncTask::submit(slot->value.kernelDriver->subjectCode(), 0, &kfsArgs,
                                               /*autoFree=*/false);
        if (!fsTask) {
            args->bytesWritten = 0;
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        fsTask->submitterTask = task->submitterTask;  // 실측 발견, PN-EA4EE935 - OpenHandler 주석 참고
        AsyncTaskAwaiter(fsTask).await();

        if (kfsArgs.error != VfsError::None) {
            // [실제로 관측 예정] 현재 유일한 KernelFsDriver 구현체인
            // LiveFs는 세 경로 전부 읽기 전용이라 항상 PermissionDenied를
            // 반환한다(mount_table.h "v1 축소 범위" 절 그대로) - 이
            // 핸들러는 그 응답을 있는 그대로 전달할 뿐 별도 처리 없음.
            args->bytesWritten = 0;
            args->error = kMapVfsError(kfsArgs.error);
            co_return;
        }
        slot->value.offset += kfsArgs.bytesWritten;
        args->bytesWritten = kfsArgs.bytesWritten;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

WriteHandler gWriteHandler;

// [SP-2AAD7C8D §9.3, PN-E9960D10] Set/Current만 지원(End는 vfs_syscall.h
// 상단 주석 참고 - 정직하게 범위 밖) - fd 테이블 offset의 순수 산술이라
// KernelFsDriver 호출 자체가 필요 없다.
class LseekHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<LseekArgs*>(argsRaw);
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        const int32_t fd = args->fd;
        auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
        if (!slot) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }

        if (args->whence == SeekWhence::End) {
            args->error = ChannelError::NotSupported;  // PN-E9960D10 "왜 End를 이번에 빼는가" 참고
            co_return;
        }

        const int64_t base =
            args->whence == SeekWhence::Set ? 0 : static_cast<int64_t>(slot->value.offset);  // Current면 현재 offset 기준
        const int64_t result = base + args->offset;
        if (result < 0) {
            args->error = ChannelError::InvalidArgument;  // POSIX EINVAL과 동일한 결(음수 offset은 무효)
            co_return;
        }

        slot->value.offset = static_cast<uint64_t>(result);
        args->newOffset = slot->value.offset;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

LseekHandler gLseekHandler;

// [SP-2AAD7C8D §9.3/§9.4, PN-238FD331] Stat - fd 없이 경로만으로
// 동작(ResolvePathHandler와 거의 같은 모양). MountKind::Channel은
// Open과 동일한 이유로 아직 NotSupported.
class StatHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<StatArgs*>(argsRaw);
        if (!kValidateVfsBuffer(task, args->path, args->pathLen)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }

        MountKind kind{};
        uint64_t channelId = 0;
        KernelFsDriver* driver = nullptr;
        uint32_t relOffset = 0;
        if (!MountTable::resolve(args->path, args->pathLen, &kind, &channelId, &driver, &relOffset)) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        if (kind == MountKind::Channel) {
            args->error = ChannelError::NotSupported;  // PN-EA4EE935와 동일한 스코프 결정
            co_return;
        }

        KernelFsStatArgs kfsArgs;
        kfsArgs.relPath = args->path + relOffset;
        kfsArgs.relPathLen = args->pathLen - relOffset;
        AsyncTask* fsTask = AsyncTask::submit(driver->subjectCode(), 0, &kfsArgs, /*autoFree=*/false);
        if (!fsTask) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        fsTask->submitterTask = task->submitterTask;  // PN-EA4EE935 실측 발견 그대로 재적용
        AsyncTaskAwaiter(fsTask).await();

        if (kfsArgs.error != VfsError::None) {
            args->error = kMapVfsError(kfsArgs.error);
            co_return;
        }
        args->size = kfsArgs.size;
        args->isDirectory = kfsArgs.isDirectory;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

StatHandler gStatHandler;

}  // namespace

void VfsSyscallService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointMount, &gMountHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointUnmount, &gUnmountHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointResolvePath, &gResolvePathHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSignalUserlandReady, &gSignalUserlandReadyHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointWaitForUserlandReady, &gWaitForUserlandReadyHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointOpen, &gOpenHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointClose, &gCloseHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointRead, &gReadHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointWrite, &gWriteHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointLseek, &gLseekHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointStat, &gStatHandler);
}

}  // namespace kernel
