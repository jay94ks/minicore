#include "authmgr_client.h"

#include "async_task.h"
#include "libkenv/mem.h"
#include "named_object.h"
#include "scheduler.h"
#include "task.h"

namespace kernel {

namespace {

// [DC-90A66932] userland/libs/libmc/authmgr.h의 mc::AuthmgrMessageHeader/
// AuthmgrRequestHeader/AuthmgrResponseHeader/AuthmgrUserRecord와 필드
// 순서/크기 1:1로 일치하는 커널 쪽 와이어 미러 - 커널 빌드 트리는
// 유저랜드 전용인 libmc를 직접 include할 수 없어(별개 툴체인) 여기
// 다시 정의한다. **한쪽을 바꾸면 반드시 다른 쪽도 함께 바꿀 것.**
enum class WireFrameKind : uint8_t { Request = 1, Response = 2, Notification = 3 };
enum class WireRequestType : uint8_t {
    Ping = 1,
    LookupByUid = 2,
    CreateUser = 3,
    CheckSudoPermission = 4,
    GrantSudoPermission = 5,
};

struct WireMessageHeader {
    uint32_t totalLength = 0;
    WireFrameKind frameKind = WireFrameKind::Request;
    uint8_t reserved[3] = {};
};
struct WireRequestHeader {
    WireMessageHeader header;
    WireRequestType requestType = WireRequestType::Ping;
    uint8_t reserved[3] = {};
};
struct WireLookupByUidRequestBody {
    uint32_t uid = 0;
};
struct WireSudoPermissionRequestBody {
    uint32_t callerUid = 0;
    uint32_t targetUid = 0;
};
struct WireResponseHeader {
    WireMessageHeader header;
    WireRequestType requestType = WireRequestType::Ping;
    uint8_t reserved[3] = {};
    uint32_t error = 0;
};
struct WireUserRecord {
    uint32_t uid = 0;
    uint32_t parentUid = 0;
    uint32_t gid = 0;
    char loginName[kUserRecordMaxLoginNameBytes] = {};
    char passwordHash[kUserRecordMaxPasswordHashBytes] = {};
    char defaultShell[kUserRecordMaxShellBytes] = {};
};
static_assert(sizeof(WireUserRecord) == 4 + 4 + 4 + kUserRecordMaxLoginNameBytes + kUserRecordMaxPasswordHashBytes +
                                             kUserRecordMaxShellBytes,
              "WireUserRecord는 패딩 없이 mc::AuthmgrUserRecord와 바이트 단위로 일치해야 한다");

constexpr char kAuthmgrChannelName[] = "authmgr";
constexpr uint32_t kAuthmgrChannelNameLen = sizeof(kAuthmgrChannelName) - 1;

SharedPtr<Channel> kResolveAuthmgrChannel() {
    NamedObjectKind kind{};
    uint64_t objectId = 0;
    if (!NamedObjectTable::resolve(kAuthmgrChannelName, kAuthmgrChannelNameLen, &kind, &objectId) ||
        kind != NamedObjectKind::Channel) {
        return SharedPtr<Channel>();
    }
    return kResolveChannelId(static_cast<ChannelId>(objectId));
}

struct AuthmgrConnectArgs {
    ChannelError error = ChannelError::ServiceUnavailable;
    // [수정, 2026-09-28, 실측으로 발견한 UAF 수정] 예전엔 여기 raw
    // `BridgePipe*`(req.resultBridge 그대로)만 담아 뒀다가 이 코루틴이
    // 끝난(=`connectTask`가 `AsyncTaskCoroAwaiter::await_resume()`으로
    // 이미 반납된) 한참 뒤 `kAuthmgrFinishConnect()`에서 그 해제된
    // `connectTask`를 다시 `kResolveOwnedBridge(connectTask, ...)`에
    // 넘겨 `submitterTask`를 읽으려 했다 - use-after-free(setuidtest
    // E2E 검증 중 조용한 행업으로 실측). `kResolveOwnedBridge()`가
    // 필요로 하는 살아있는 `AsyncTask*`(이 핸들러 자신의 `task`)는
    // 오직 이 `onExec()` 안에서만 유효하므로, 검증까지 여기서 끝내고
    // 이미 검증된 `SharedPtr`만 넘긴다.
    SharedPtr<BridgePipe> resolvedBridge;
};

// channel.cpp의 ConnectChannelHandler::onExec/onCancel과 동일한 로직
// (busy-yield 핸드셰이크) - kValidateUserBuffer 호출만 없다(이름이
// 컴파일 타임 상수 문자열이라 검증 대상 자체가 없음).
class AuthmgrConnectHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<AuthmgrConnectArgs*>(argsRaw);
        SharedPtr<Channel> channel = kResolveAuthmgrChannel();
        if (!channel) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        PendingConnectRequest req;
        req.task = task;
        req.useHugePage = false;

        AsyncTask* accepter = nullptr;
        {
            SpinlockGuard guard(channel->lock);
            if (channel->destroyed) {
                args->error = ChannelError::NotFound;
                co_return;
            }
            channel->pushPendingConnect(&req);
            accepter = channel->pendingAccepters.popFront();
        }
        if (accepter) {
            AsyncReactor::submitCompletion(accepter, channel->exclusivePreemptive);
        }

        while (req.done.load() == 0) {
            AsyncTask::yield();
        }

        if (req.rejected) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        // AcceptFromChannelHandler(channel.cpp)가 req.done을 세우기
        // 전에 이미 clientSide를 우리(gAuthmgrClientOwner)의
        // openBridges에 넣어 뒀다(channel.cpp의 clientBridges->insert()
        // 가 req->done.store(1)보다 먼저 실행되는 순서 - 실제 소스로
        // 확인) - 그러니 지금(이 코루틴이 아직 `task`=`connectTask`로
        // 살아있는 이 시점) 바로 정식 검증까지 끝낸다(위 AuthmgrConnectArgs
        // 문서 주석 참고 - 이후로 미루면 UAF).
        args->resolvedBridge = kResolveOwnedBridge(task, reinterpret_cast<uint64_t>(req.resultBridge));
        args->error = args->resolvedBridge ? ChannelError::None : ChannelError::ServiceUnavailable;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<AuthmgrConnectArgs*>(argsRaw);
        args->error = ChannelError::Interrupted;
        SharedPtr<Channel> channel = kResolveAuthmgrChannel();
        if (!channel) {
            return;
        }
        SpinlockGuard guard(channel->lock);
        if (channel->destroyed) {
            return;
        }
        channel->removePendingConnect(task);
    }
};

AuthmgrConnectHandler gAuthmgrConnectHandler;
AsyncTaskSubjectCode gAuthmgrConnectSubjectCode = 0;
AuthmgrConnectArgs gAuthmgrConnectArgs;  // kAuthmgrClientLock()이 동시 호출을 직렬화하므로 단일 인스턴스로 충분

SharedPtr<BridgePipe> gAuthmgrBridge;
KernelThread* gAuthmgrClientOwner = nullptr;

bool gAuthmgrClientBusy = false;
Spinlock gAuthmgrClientBusyGuardLock;

// 이 오너 KernelThread는 실제 작업을 하지 않는다 - openBridges를 가질
// 수 있는 안정적인 Task 신원으로 존재하는 것 자체가 유일한 목적이다
// (위 authmgr_client.h 문서 주석 참고).
void kAuthmgrClientOwnerMain(void*) {
    for (;;) {
        Scheduler::yieldCurrent();
    }
}

}  // namespace

void kInitAuthmgrClient() {
    gAuthmgrConnectSubjectCode = AsyncCallbackRegistry::registerHandler(&gAuthmgrConnectHandler);
    gAuthmgrClientOwner = kSpawnKernelThread(kAuthmgrClientOwnerMain, nullptr);
    if (gAuthmgrClientOwner) {
        Scheduler::enqueue(Scheduler::currentCoreIndex(), gAuthmgrClientOwner);
    }
}

void kAuthmgrClientLock() {
    for (;;) {
        bool acquired = false;
        {
            SpinlockGuard guard(gAuthmgrClientBusyGuardLock);
            if (!gAuthmgrClientBusy) {
                gAuthmgrClientBusy = true;
                acquired = true;
            }
        }
        if (acquired) {
            return;
        }
        AsyncTask::yield();
    }
}

void kAuthmgrClientUnlock() {
    SpinlockGuard guard(gAuthmgrClientBusyGuardLock);
    gAuthmgrClientBusy = false;
}

AsyncTask* kAuthmgrBeginConnect() {
    if (gAuthmgrBridge) {
        return nullptr;
    }
    if (!gAuthmgrClientOwner) {
        return nullptr;  // kInitAuthmgrClient()가 아직 안 불렸거나 스폰 실패(부팅 중이 아니면 있을 수 없음)
    }
    gAuthmgrConnectArgs = AuthmgrConnectArgs{};
    // [Syscall::submit()과 동일한 관례, syscall.cpp 참고] 제출 직후
    // submitterTask를 채우기 전까지 이 코어의 리액터가 끼어들어 이
    // AsyncTask를 먼저 실행해 버리면 안 되므로 그 구간 전체를 선점
    // 금지로 감싼다.
    PreemptionGuard guard;
    AsyncTask* task =
        AsyncTask::submit(gAuthmgrConnectSubjectCode, 0, &gAuthmgrConnectArgs, /*autoFree=*/false, /*preemptive=*/true);
    if (!task) {
        return nullptr;
    }
    task->submitterTask = TaskOwnerRef::capture(gAuthmgrClientOwner->weakAsTask());
    return task;
}

bool kAuthmgrFinishConnect(AsyncTask* connectTask) {
    // [수정, 2026-09-28, 실측 UAF 수정] `connectTask`는 호출부(user_record.cpp
    // kSetuidOnExecImpl)가 이 함수를 부르기 직전 `co_await
    // AsyncTaskCoroAwaiter(connectTask)`로 이미 `await_resume()` 안에서
    // `kReleaseAsyncTask()`를 거쳐 반납한 뒤다(async_task.h 문서 주석
    // "await_resume()이 직접 반납한다") - 여기서 또 반납하면 이중 해제,
    // 그리고 애초에 `connectTask`를 역참조하는 건(구 코드의
    // `kResolveOwnedBridge(connectTask, ...)`) 전부 use-after-free다.
    // 검증된 브리지는 이미 AuthmgrConnectHandler::onExec()이 살아있는
    // 동안 `gAuthmgrConnectArgs.resolvedBridge`에 담아 뒀으므로 그걸
    // 그대로 옮겨 받기만 한다 - `connectTask` 자체는 이제 안 쓴다
    // (시그니처는 호출부와의 대칭을 위해 유지).
    (void)connectTask;
    bool ok = false;
    if (gAuthmgrConnectArgs.error == ChannelError::None && gAuthmgrConnectArgs.resolvedBridge) {
        gAuthmgrBridge = gAuthmgrConnectArgs.resolvedBridge;
        ok = true;
    }
    gAuthmgrConnectArgs.resolvedBridge.reset();
    return ok;
}

bool kAuthmgrEnsureConnected() {
    AsyncTask* connectTask = kAuthmgrBeginConnect();
    if (!connectTask) {
        return static_cast<bool>(gAuthmgrBridge);  // 이미 연결돼 있었거나(true) 오너 미초기화(false)
    }
    // authmgr_client.h의 kAuthmgrBeginConnect() 문서 주석 참고 - 여기도
    // co_await가 아니라 AsyncTaskAwaiter로 기다린다.
    AsyncTaskAwaiter(connectTask).await();
    return kAuthmgrFinishConnect(connectTask);
}

namespace {

// channel.cpp의 ChannelWriteHandler::onExec와 동일한 busy-yield 쓰기
// 루프 - kValidateUserBuffer/kResolveOwnedBridge 둘 다 없다(buf는
// 호출부 자신의 커널 스택, gAuthmgrBridge는 이미 검증된 채로 들고
// 있음). kAuthmgrReadAll과 대칭인 범용 헬퍼 - 원래
// kAuthmgrWriteLookupRequest 안에 있던 것을 뽑아 sudo 화이트리스트
// 질의(kAuthmgrCheckSudoPermission)와 공유한다.
bool kAuthmgrWriteAll(const uint8_t* buf, uint64_t length) {
    if (!gAuthmgrBridge) {
        return false;
    }
    AsyncTask* task = AsyncTask::current();
    if (!task) {
        return false;
    }
    RingBuffer& ring = gAuthmgrBridge->outbound;
    uint64_t sent = 0;
    while (sent < length) {
        AsyncTask* wakeReader = nullptr;
        bool broken = false;
        bool progressed = false;
        {
            SpinlockGuard guard(ring.lock);
            if (kIsBridgeBroken(gAuthmgrBridge.get())) {
                broken = true;
            } else {
                const uint64_t space = ring.capacity - ring.used;
                if (space > 0) {
                    const uint64_t remaining = length - sent;
                    const uint64_t n = space < remaining ? space : remaining;
                    for (uint64_t i = 0; i < n; ++i) {
                        ring.data[(ring.writePos + i) % ring.capacity] = buf[sent + i];
                    }
                    ring.writePos = (ring.writePos + n) % ring.capacity;
                    ring.used += n;
                    sent += n;
                    wakeReader = ring.pendingReaders.popFront();
                    progressed = true;
                } else {
                    ring.pendingWriters.pushBack(task);
                }
            }
        }
        if (wakeReader) {
            AsyncReactor::submitCompletion(wakeReader);
        }
        if (broken) {
            gAuthmgrBridge.reset();
            return false;
        }
        if (!progressed) {
            AsyncTask::yield();
        }
    }
    return true;
}

}  // namespace

bool kAuthmgrWriteLookupRequest(Uid uid) {
    WireRequestHeader header{};
    header.header.totalLength = sizeof(WireRequestHeader) + sizeof(WireLookupByUidRequestBody);
    header.header.frameKind = WireFrameKind::Request;
    header.requestType = WireRequestType::LookupByUid;
    WireLookupByUidRequestBody body{};
    body.uid = uid;

    uint8_t buf[sizeof(WireRequestHeader) + sizeof(WireLookupByUidRequestBody)];
    memcpy(buf, &header, sizeof(header));
    memcpy(buf + sizeof(header), &body, sizeof(body));
    return kAuthmgrWriteAll(buf, sizeof(buf));
}

namespace {

// channel.cpp의 ChannelReadHandler::onExec와 동일한 busy-yield 읽기
// 루프 - length바이트를 전부 채울 때까지 반복한다(원 핸들러는 한 번의
// syscall 호출이 "그만큼만" 읽고 돌아가지만, 우리는 고정 크기 응답을
// 한 번에 전부 받아야 하므로 호출부를 대신해 여기서 반복한다).
bool kAuthmgrReadAll(uint8_t* outBuf, uint64_t length) {
    if (!gAuthmgrBridge) {
        return false;
    }
    AsyncTask* task = AsyncTask::current();
    if (!task) {
        return false;
    }
    SharedPtr<BridgePipe> peer = gAuthmgrBridge->peer.lock();
    if (!peer) {
        gAuthmgrBridge.reset();
        return false;
    }
    RingBuffer& ring = peer->outbound;
    uint64_t received = 0;
    while (received < length) {
        AsyncTask* wakeWriter = nullptr;
        bool broken = false;
        bool progressed = false;
        {
            SpinlockGuard guard(ring.lock);
            if (ring.used > 0) {
                const uint64_t remaining = length - received;
                const uint64_t n = ring.used < remaining ? ring.used : remaining;
                for (uint64_t i = 0; i < n; ++i) {
                    outBuf[received + i] = ring.data[(ring.readPos + i) % ring.capacity];
                }
                ring.readPos = (ring.readPos + n) % ring.capacity;
                ring.used -= n;
                received += n;
                wakeWriter = ring.pendingWriters.popFront();
                progressed = true;
            } else if (kIsBridgeBroken(gAuthmgrBridge.get())) {
                broken = true;
            } else {
                ring.pendingReaders.pushBack(task);
            }
        }
        if (wakeWriter) {
            AsyncReactor::submitCompletion(wakeWriter);
        }
        if (broken) {
            gAuthmgrBridge.reset();
            return false;
        }
        if (!progressed) {
            AsyncTask::yield();
        }
    }
    return true;
}

}  // namespace

bool kAuthmgrReadLookupResponse(UserRecord* outRecord) {
    WireResponseHeader header{};
    if (!kAuthmgrReadAll(reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
        return false;
    }
    if (header.header.frameKind != WireFrameKind::Response || header.requestType != WireRequestType::LookupByUid ||
        header.header.totalLength < sizeof(WireResponseHeader)) {
        // 프로토콜이 어긋났다 - 스트림 동기화가 깨졌을 가능성이 높아
        // 재연결을 유도한다(이 자리에서 남은 바이트를 그냥 버리고
        // 계속 쓰는 건 더 위험하다).
        gAuthmgrBridge.reset();
        return false;
    }
    if (header.error != 0) {
        return false;  // NotFound 등 - 연결 자체는 정상, 재연결 불필요
    }
    const uint32_t bodyLen = header.header.totalLength - sizeof(WireResponseHeader);
    WireUserRecord wire{};
    if (bodyLen != sizeof(WireUserRecord)) {
        gAuthmgrBridge.reset();
        return false;
    }
    if (!kAuthmgrReadAll(reinterpret_cast<uint8_t*>(&wire), sizeof(wire))) {
        return false;
    }
    outRecord->uid = wire.uid;
    outRecord->parentUid = wire.parentUid;
    outRecord->gid = wire.gid;
    memcpy(outRecord->loginName, wire.loginName, sizeof(outRecord->loginName));
    memcpy(outRecord->passwordHash, wire.passwordHash, sizeof(outRecord->passwordHash));
    memcpy(outRecord->defaultShell, wire.defaultShell, sizeof(outRecord->defaultShell));
    return true;
}

// [신규, 2026-09-28, DC-34764C25 항목1 답변] write+read를 한 번에
// 묶은 왕복 - LookupByUid와 달리 호출부(kSetuidOnExecImpl)가 응답
// 본문을 따로 채울 게 없어(허용 여부 하나뿐) 별도 write/read 함수
// 쌍으로 안 쪼갠다. 통신 오류는 전부 false(불허)로 접는다 - 권한
// 승격 경로라 "확실히 허용됨"이 확인된 경우에만 true를 반환해야
// 한다(fail-closed, kAuthmgrReadLookupResponse의 "실패=재시도 유도"
// 관례와 달리 여기는 "실패=거부"가 맞다).
bool kAuthmgrCheckSudoPermission(Uid callerUid, Uid targetUid) {
    WireRequestHeader header{};
    header.header.totalLength = sizeof(WireRequestHeader) + sizeof(WireSudoPermissionRequestBody);
    header.header.frameKind = WireFrameKind::Request;
    header.requestType = WireRequestType::CheckSudoPermission;
    WireSudoPermissionRequestBody body{};
    body.callerUid = callerUid;
    body.targetUid = targetUid;

    uint8_t buf[sizeof(WireRequestHeader) + sizeof(WireSudoPermissionRequestBody)];
    memcpy(buf, &header, sizeof(header));
    memcpy(buf + sizeof(header), &body, sizeof(body));
    if (!kAuthmgrWriteAll(buf, sizeof(buf))) {
        return false;
    }

    WireResponseHeader response{};
    if (!kAuthmgrReadAll(reinterpret_cast<uint8_t*>(&response), sizeof(response))) {
        return false;
    }
    if (response.header.frameKind != WireFrameKind::Response ||
        response.requestType != WireRequestType::CheckSudoPermission ||
        response.header.totalLength != sizeof(WireResponseHeader)) {
        // 프로토콜이 어긋났다 - kAuthmgrReadLookupResponse와 동일한
        // 이유로 재연결을 유도한다.
        gAuthmgrBridge.reset();
        return false;
    }
    return response.error == 0;  // 0 == mc::ChannelError::None
}

// [신규, 2026-09-28, DC-CC83F7BE 답변 반영] CreateUser 요청 - 응답
// 본문 없음(error만, userland/libs/libmc/authmgr.h §CreateUser 주석과
// 동일한 관례) - kAuthmgrCheckSudoPermission과 동일한 write+read 왕복
// 모양.
bool kAuthmgrCreateUser(const UserRecord& record) {
    WireRequestHeader header{};
    header.header.totalLength = sizeof(WireRequestHeader) + sizeof(WireUserRecord);
    header.header.frameKind = WireFrameKind::Request;
    header.requestType = WireRequestType::CreateUser;
    WireUserRecord body{};
    body.uid = record.uid;
    body.parentUid = record.parentUid;
    body.gid = record.gid;
    memcpy(body.loginName, record.loginName, sizeof(body.loginName));
    memcpy(body.passwordHash, record.passwordHash, sizeof(body.passwordHash));
    memcpy(body.defaultShell, record.defaultShell, sizeof(body.defaultShell));

    uint8_t buf[sizeof(WireRequestHeader) + sizeof(WireUserRecord)];
    memcpy(buf, &header, sizeof(header));
    memcpy(buf + sizeof(header), &body, sizeof(body));
    if (!kAuthmgrWriteAll(buf, sizeof(buf))) {
        return false;
    }

    WireResponseHeader response{};
    if (!kAuthmgrReadAll(reinterpret_cast<uint8_t*>(&response), sizeof(response))) {
        return false;
    }
    if (response.header.frameKind != WireFrameKind::Response || response.requestType != WireRequestType::CreateUser ||
        response.header.totalLength != sizeof(WireResponseHeader)) {
        gAuthmgrBridge.reset();
        return false;
    }
    return response.error == 0;
}

}  // namespace kernel
