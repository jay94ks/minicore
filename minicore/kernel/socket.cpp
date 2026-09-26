#include "socket.h"

#include "async_task.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "named_object.h"
#include "paging.h"
#include "process.h"

namespace kernel {

namespace {

// [socket.h §4-1 포맷 주석 참고] 이 두 헬퍼는 순수 문자열 조립이라
// 드리프트 위험이 없다 - resource_group.cpp/procfs.cpp와 동일한
// 최소 구현을 그대로 복제한다(Logger 포맷터는 재사용 불가능한
// 캡슐화라 각 파일이 자신만의 좁은 용도로 다시 최소 구현하는 이
// 프로젝트의 기존 관례).
void kAppendStr(char* buf, uint32_t bufSize, uint32_t& pos, const char* s) {
    while (*s && pos + 1 < bufSize) {
        buf[pos++] = *s++;
    }
}

void kAppendI64(char* buf, uint32_t bufSize, uint32_t& pos, int64_t value) {
    const bool neg = value < 0;
    const uint64_t mag = neg ? (~static_cast<uint64_t>(value) + 1) : static_cast<uint64_t>(value);
    char digits[24];
    uint32_t n = 0;
    uint64_t m = mag;
    if (m == 0) {
        digits[n++] = '0';
    }
    while (m) {
        digits[n++] = static_cast<char>('0' + (m % 10));
        m /= 10;
    }
    if (neg) {
        kAppendStr(buf, bufSize, pos, "-");
    }
    while (n) {
        char c[2] = {digits[--n], '\0'};
        kAppendStr(buf, bufSize, pos, c);
    }
}

// [channel.cpp의 kProcessFromSubmitter/vfs_syscall.cpp의 동일 함수와
// 같은 패턴 재사용 - 각 파일이 자기 몫을 따로 갖는 기존 관례] v1은
// 소켓 syscall이 전부 유저 트랩 경로로만 도달한다(Process::fileDescriptors
// 자체가 Process 전용, KernelThread엔 없음) - KernelThread 제출자는
// 안전하게 거절한다(vfs_syscall.cpp의 kValidateVfsBuffer가 채택한
// PN-A8BE8BED 항목4의 더 최신/안전한 관례를 그대로 따름).
SharedPtr<Process> kProcessFromSubmitter(AsyncTask* task) {
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter || !submitter->isUserLevel) {
        return SharedPtr<Process>();
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return thread->process.lock();
}

bool kValidateUserBuffer(AsyncTask* task, const void* ptr, uint64_t length) {
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter || !submitter->isUserLevel) {
        return false;
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return Paging::isUserRangeValid(reinterpret_cast<uint64_t>(ptr), length, thread->userPml4Phys);
}

// [PN-EA4EE935 원안, vfs_syscall.cpp와 동일한 최소 구현 복제] 이
// 프로세스에서 아직 안 쓰는 가장 작은 fd - Socket()도 같은
// `Process::fileDescriptors` 공간에서 fd를 배정해야 하므로 정확히
// 같은 스캔 로직이 필요하다(둘 다 매번 그 순간의 `fileDescriptors`를
// 다시 훑을 뿐 상태를 캐시하지 않아, 두 복제본이 있어도 결과가 갈릴
// 여지가 없다 - kAppendStr류와 같은 "안전하게 중복 가능한 헬퍼").
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

// socket.h 문서 주석(SP-231493CB §3) 그대로 - 이 파일의 모든 핸들러가
// 실제 랑데부/링버퍼 로직을 새로 구현하지 않고, 이미 등록된 Channel
// syscall 핸들러를 SyscallEndpointId로 재귀 호출한다(vfs_syscall.cpp의
// StatHandler가 KernelFsDriver에 재제출하는 것과 동일한 합성 패턴 -
// 그 핸들러가 이미 실전 검증된 `AsyncTaskAwaiter(nested).await()`
// 관용구를 그대로 따른다, `AsyncTaskCoroAwaiter`의 `co_await`가 아님에
// 주의 - 이 파일의 onExec들도 StatHandler와 동일하게 코루틴
// (AsyncExecCoro)이면서 이 관용구를 쓴다). submitterTask를 원래
// 호출자로 그대로 물려줘야 그 안쪽 핸들러들(kOwnerOpenBridgesOf/
// kProcessFromSubmitter 등)이 "진짜 호출자"를 올바르게 식별한다.
bool kSubmitAndAwait(AsyncTask* callerTask, SyscallEndpointId endpointId, void* args) {
    AsyncTaskSubjectCode subjectCode;
    if (!SyscallRegistry::resolveSubjectCode(endpointId, &subjectCode)) {
        return false;
    }
    AsyncTask* nested = AsyncTask::submit(subjectCode, 0, args, /*autoFree=*/false);
    if (!nested) {
        return false;
    }
    nested->submitterTask = callerTask->submitterTask;
    AsyncTaskAwaiter(nested).await();
    return true;
}

class SocketHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SocketArgs*>(argsRaw);
        if (args->domain != SocketDomain::Unix) {
            args->error = ChannelError::NotSupported;  // v1은 AF_UNIX만(SP-231493CB §2)
            co_return;
        }
        if (args->type != SocketType::Stream && args->type != SocketType::Datagram) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }

        // §3 - Socket()은 항상 익명 Channel을 하나 만든다(§4-1 자동
        // 등록 경로의 실체) - 기존 OpenChannel 핸들러를 그대로 위임.
        OpenChannelArgs openArgs;
        if (!kSubmitAndAwait(task, kSyscallEndpointOpenChannel, &openArgs)) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        if (openArgs.error != ChannelError::None) {
            args->error = openArgs.error;
            co_return;
        }

        auto* socket = static_cast<UnixSocket*>(GenericSlabAllocator::alloc(sizeof(UnixSocket)));
        if (!socket) {
            DestroyChannelArgs destroyArgs;
            destroyArgs.channelHandle = openArgs.channelHandle;
            kSubmitAndAwait(task, kSyscallEndpointDestroyChannel, &destroyArgs);
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        memset(socket, 0, sizeof(UnixSocket));
        socket->type = args->type;
        socket->channelId = openArgs.channelId;

        process->fileDescriptors.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        const int32_t newFd = kAllocateFd(process.get());
        if (newFd < 0) {
            GenericSlabAllocator::free(socket, sizeof(UnixSocket));
            DestroyChannelArgs destroyArgs;
            destroyArgs.channelHandle = openArgs.channelHandle;
            kSubmitAndAwait(task, kSyscallEndpointDestroyChannel, &destroyArgs);
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }

        Process::FileDescriptor fdEntry;
        fdEntry.fd = newFd;
        fdEntry.kind = MountKind::Socket;
        fdEntry.socket = socket;
        fdEntry.used = true;
        process->fileDescriptors.insert(fdEntry);

        // [SP-231493CB §4-1] 모든 소켓의 자동 등록 경로 - "<pid>/<handle>".
        // [정직하게 기록] 커널/커널 서비스가 직접 소켓을 만드는
        // "unix/kernel/<ownerId>/<handle>" 갈래는 실사용처가 없다
        // (Process::fileDescriptors 자체가 Process 전용, KernelThread엔
        // 없음) - 그 경로가 실제로 필요해지는 시점으로 미룬다
        // (RM-23F4B687 §4).
        char autoPath[kMaxNamedObjectNameLength];
        const uint32_t autoPathLen =
            kFormatAutoSocketPath(process->processId, newFd, autoPath, kMaxNamedObjectNameLength);
        // [정직하게 기록] 이 reserve()가 실패할 수 있는 유일한 경우는
        // NamedObjectTable 슬롯 고갈(128개, kMaxNamedObjects)뿐이다
        // (pid/fd 조합은 항상 유일해 이름 충돌은 불가능) - 실패해도
        // 소켓 자체는 정상 동작한다, 단지 이 자동 가시성 목록에서
        // 안 보일 뿐이라 반환값을 확인하지 않는다(§4-1은 "발견
        // 가능성"을 위한 것이지 소켓 동작의 필수 조건이 아니다).
        NamedObjectTable::reserve(autoPath, autoPathLen, NamedObjectKind::Channel, socket->channelId);

        args->fd = newFd;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    // [OpenChannelHandler와 동일한 이유] 이 onExec은 co_await 지점이
    // 전혀 없다(kSubmitAndAwait는 일반 함수 호출이지 co_await가 아님) -
    // 항상 한 번에 끝까지 실행되므로 취소는 실행 전에만 가능하고 그
    // 시점엔 아무 자원도 안 만들어졌다.
    void onCancel(AsyncTask*, void*) override {}
};

class SocketBindHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SocketBindArgs*>(argsRaw);
        if (args->pathLen == 0 || args->pathLen > kMaxNamedObjectNameLength) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }
        if (!kValidateUserBuffer(task, args->path, args->pathLen)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        // [socket.h 문서 주석] v1은 '/' 없는 단순 이름만 - 실제 VFS
        // 경로 바인드(§4-2 항목2/3)는 이번 증분 범위 밖.
        if (args->path[0] == '/') {
            args->error = ChannelError::NotSupported;
            co_return;
        }
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        const int32_t fd = args->fd;
        auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
        if (!slot || slot->value.kind != MountKind::Socket) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        UnixSocket* socket = slot->value.socket;
        if (socket->channelId == 0) {
            // Accept()가 만든(자기 Channel이 없는, 이미 연결된) 소켓 -
            // bind 대상이 될 수 없다.
            args->error = ChannelError::InvalidArgument;
            co_return;
        }
        if (socket->explicitlyBound) {
            // POSIX의 "이미 bind된 소켓 재bind 금지" 관례(socket.h
            // UnixSocket::boundPath 문서 주석 참고).
            args->error = ChannelError::InvalidArgument;
            co_return;
        }

        // [kCreateNamedChannel(channel.cpp)과 동일한 관례] reserve()는
        // 순수 동기 함수라 유효성 검증만 이미 끝났다면 유저 포인터를
        // 그대로 넘겨도 안전하다(그 함수가 스핀락을 쥔 채 즉시
        // memcmp/memcpy하고 반환) - 별도 커널 버퍼로 복사할 필요 없음.
        if (!NamedObjectTable::reserve(args->path, args->pathLen, NamedObjectKind::Channel, socket->channelId)) {
            args->error = ChannelError::NameInUse;
            co_return;
        }
        // 이름 자체는 소켓 수명 내내 쓸 수 있어야 하므로(Close 시
        // 반납) 여기서만 커널 쪽 사본을 남긴다 - 위 reserve() 호출과는
        // 무관한, 순수 북키핑 목적의 복사.
        socket->explicitlyBound = true;
        memcpy(socket->boundPath, args->path, args->pathLen);
        socket->boundPathLength = args->pathLen;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class SocketListenHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SocketListenArgs*>(argsRaw);
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        const int32_t fd = args->fd;
        auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
        if (!slot || slot->value.kind != MountKind::Socket) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        UnixSocket* socket = slot->value.socket;
        if (socket->type != SocketType::Stream) {
            args->error = ChannelError::NotSupported;  // Datagram엔 listen/accept 개념이 없다(§8 항목2)
            co_return;
        }
        if (socket->channelId == 0) {
            args->error = ChannelError::InvalidArgument;  // Accept()가 만든 소켓 - 자기 Channel이 없어 listen 불가
            co_return;
        }
        socket->listening = true;
        socket->backlog = args->backlog;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class SocketAcceptHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SocketAcceptArgs*>(argsRaw);
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        const int32_t fd = args->fd;
        auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
        if (!slot || slot->value.kind != MountKind::Socket) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        UnixSocket* listener = slot->value.socket;
        if (listener->type != SocketType::Stream || !listener->listening || listener->channelId == 0) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }

        // §5 - 연결 대기까지 블로킹. AcceptFromChannel 자신이 이미
        // 그 블로킹 루프(AsyncTask::yield() 기반)를 구현하고 있어
        // 그대로 위임한다.
        AcceptFromChannelArgs acceptArgs;
        acceptArgs.channelHandle = listener->channelId;
        if (!kSubmitAndAwait(task, kSyscallEndpointAcceptFromChannel, &acceptArgs)) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        if (acceptArgs.error != ChannelError::None) {
            args->error = acceptArgs.error;
            co_return;
        }

        // [정직하게 기록] Accept()가 낳은 소켓은 자기 자신의 Channel이
        // 없다(channelId=0으로 유지, socket.h UnixSocket 문서 주석
        // 참고) - §4-1 자동 등록도 하지 않는다(그 절은 명시적으로
        // "Socket()으로 만들어지는" 소켓만 대상으로 한다, 연결마다
        // 계속 쌓이는 무의미한 이름 항목을 막기 위함).
        auto* connected = static_cast<UnixSocket*>(GenericSlabAllocator::alloc(sizeof(UnixSocket)));
        if (!connected) {
            CloseBridgeArgs closeArgs;
            closeArgs.bridge = acceptArgs.bridge;
            kSubmitAndAwait(task, kSyscallEndpointCloseBridge, &closeArgs);
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        memset(connected, 0, sizeof(UnixSocket));
        connected->type = listener->type;
        connected->bridge = acceptArgs.bridge;

        process->fileDescriptors.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        const int32_t newFd = kAllocateFd(process.get());
        if (newFd < 0) {
            GenericSlabAllocator::free(connected, sizeof(UnixSocket));
            CloseBridgeArgs closeArgs;
            closeArgs.bridge = acceptArgs.bridge;
            kSubmitAndAwait(task, kSyscallEndpointCloseBridge, &closeArgs);
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }

        Process::FileDescriptor fdEntry;
        fdEntry.fd = newFd;
        fdEntry.kind = MountKind::Socket;
        fdEntry.socket = connected;
        fdEntry.used = true;
        process->fileDescriptors.insert(fdEntry);

        args->newFd = newFd;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    // [ConnectChannelHandler/AcceptFromChannelHandler와 동일한 이유]
    // 이 onExec은 kSubmitAndAwait(정확히는 그 안의 AcceptFromChannel
    // 재제출)이 co_await 없이 블로킹하는 동안 취소될 수 있다 - 하지만
    // 그 블로킹은 AcceptFromChannelHandler 자신의 AsyncTask(위 nested)
    // 위에서 일어나고, 이 바깥쪽 SocketAcceptHandler의 AsyncTask
    // (task) 자신은 그동안 어떤 대기열에도 매달리지 않는다(co_await가
    // 없어 실행이 끊기지 않고 nested가 끝날 때까지 그대로 스택에 머문다)
    // - 취소 시 정리할 이 계층만의 자원이 없다(nested 쪽 취소는 그
    // AsyncTask 자신의 onCancel이 아니라 애초에 disconnect 대상이 아님 -
    // task/nested가 서로 다른 AsyncTask이므로 이 task가 취소돼도
    // nested는 별개로 계속 실행된다는 뜻이라, 정직하게 기록해 둔다:
    // 이 취소 지원은 미완성이다 - 실사용에서 SocketAccept 대기 중
    // Kill이 걸리는 경로는 아직 실측하지 않았다, PN-CC0F4EAC 후속에서
    // 검증 필요).
    void onCancel(AsyncTask*, void*) override {}
};

class SocketConnectHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SocketConnectArgs*>(argsRaw);
        if (args->pathLen == 0 || args->pathLen > kMaxNamedObjectNameLength) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }
        if (!kValidateUserBuffer(task, args->path, args->pathLen)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        if (args->path[0] == '/') {
            args->error = ChannelError::NotSupported;  // SocketBindHandler와 동일한 v1 제약
            co_return;
        }
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        const int32_t fd = args->fd;
        auto* slot = process->fileDescriptors.find([fd](const Process::FileDescriptor& e) { return e.fd == fd; });
        if (!slot || slot->value.kind != MountKind::Socket) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        UnixSocket* socket = slot->value.socket;
        if (socket->bridge != 0) {
            args->error = ChannelError::InvalidArgument;  // 이미 연결됨(POSIX EISCONN과 동일한 취지)
            co_return;
        }
        // [정직하게 기록, 실측으로 발견] Datagram은 여기서 명시적으로
        // 거절한다 - 이 아래 ConnectChannel 핸드셰이크는 상대가
        // AcceptFromChannel을 불러야만 끝나는데, Datagram 소켓은
        // Listen()/Accept() 자체가 허용되지 않아(위 SocketListenHandler/
        // SocketAcceptHandler 참고) 아무도 그 accept를 불러 줄 수 없다 -
        // 그대로 두면 Connect()가 영원히 안 끝난다(실제로 재현해서
        // 발견). POSIX 데이터그램 소켓의 connect()는 원래 핸드셰이크가
        // 없는 순수 로컬 동작(기본 목적지만 기억)이라 Channel의
        // connect/accept 모델과 근본적으로 안 맞는다 - 새 메커니즘
        // 설계가 필요해 이번 증분 범위 밖으로 명시적으로 남긴다
        // (PN-CC0F4EAC "남은 범위" 참고, 조용히 hang하는 것보다
        // 명확한 에러가 낫다는 판단).
        if (socket->type != SocketType::Stream) {
            args->error = ChannelError::NotSupported;
            co_return;
        }

        // [중요 - 실측 전 코드 추적으로 발견] 여기서 커널 로컬 버퍼로
        // 복사한 값을 ConnectChannelArgs::name에 넘기면 안 된다 -
        // ConnectChannelHandler::onExec()이 그 포인터를 "호출자 자신의
        // 유저 주소공간에 속하는지"(kValidateUserBuffer, channel.cpp)
        // 다시 검증하는데, 그 검증은 이 nested AsyncTask의
        // submitterTask(원래 호출자로 전파됨)의 유저 주소범위를
        // 기준으로 판단한다 - 커널 스택/힙 버퍼 주소는 그 범위에 속할
        // 수 없어 항상 InvalidPointer로 실패한다. 이미 위에서 이
        // 포인터 자체를 검증했으므로, 그 원본 유저 포인터를 그대로
        // 다시 넘긴다(ConnectChannelHandler가 다시 한번 같은 검증을
        // 하는 것은 중복이지만 무해하다).
        ConnectChannelArgs connectArgs;
        connectArgs.name = args->path;
        connectArgs.nameLength = args->pathLen;
        // [정직하게 기록] useHugePage는 v1에서 항상 false - 소켓
        // syscall 어디에도 이 선택을 노출할 자리가 없다(SP-231493CB
        // §5가 열거한 시그니처 그대로, huge page는 Channel의 기존
        // 저수준 최적화 옵션이라 POSIX 소켓 API 표면에 없는 개념).
        if (!kSubmitAndAwait(task, kSyscallEndpointConnectChannel, &connectArgs)) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        if (connectArgs.error != ChannelError::None) {
            args->error = connectArgs.error;
            co_return;
        }

        socket->bridge = connectArgs.bridge;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    // [SocketAcceptHandler와 동일한 이유 - 정직하게 기록] 미완성.
    void onCancel(AsyncTask*, void*) override {}
};

SocketHandler gSocketHandler;
SocketBindHandler gSocketBindHandler;
SocketListenHandler gSocketListenHandler;
SocketAcceptHandler gSocketAcceptHandler;
SocketConnectHandler gSocketConnectHandler;

}  // namespace

uint32_t kFormatAutoSocketPath(int64_t pid, int32_t fd, char* buf, uint32_t bufCap) {
    uint32_t pos = 0;
    kAppendI64(buf, bufCap, pos, pid);
    kAppendStr(buf, bufCap, pos, "/");
    kAppendI64(buf, bufCap, pos, static_cast<int64_t>(fd));
    return pos;
}

void Socket::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointSocket, &gSocketHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSocketBind, &gSocketBindHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSocketListen, &gSocketListenHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSocketAccept, &gSocketAcceptHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSocketConnect, &gSocketConnectHandler);
    // kSyscallEndpointSocketShutdown - 아직 미구현(socket.h 문서 주석
    // 참고), 등록하지 않는다(등록 안 된 endpoint는 SyscallRegistry가
    // 이미 안전하게 NotFound류로 거절한다 - 다른 "번호만 예약" 상태
    // endpoint들과 동일한 관례).
}

}  // namespace kernel
