// minicore/sudotest - DC-34764C25 항목1(sudo/su authmgr 화이트리스트)
// E2E 검증 전용 클라이언트. setuidtest/authtest와 동일한 이유의
// out-of-tree add_subdirectory 관례 - 부팅 매니페스트에는 포함하지
// 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
//
// 시나리오: (1) 서로 무관한 두 uid(50/99, 둘 다 root의 직계 자손이라
// 조상-자손 관계가 아닌 형제)를 authmgr에 CreateUser로 만든다.
// (2) 이 프로세스는 init의 자손이라 uid=0(root)으로 시작하므로
// Setuid(50)은 root 특권으로 즉시 성공해야 한다. (3) uid=50인 채로
// Setuid(99)를 시도 - 화이트리스트에 아무것도 없으므로
// PermissionDenied로 거부돼야 한다(이게 실수로 성공하면 심각한 권한
// 상승 버그). (4) authmgr에 GrantSudoPermission(50->99)을 보낸다.
// (5) 다시 Setuid(99)를 시도 - 이번엔 kSetuidOnExecImpl의 새 sudo
// 화이트리스트 조회 경로를 타고 성공해야 한다.
//
// exitCode: 0=전 구간 성공, 1=authmgr Connect 실패, 2=CreateUser(50)
// 실패, 3=CreateUser(99) 실패, 4=Setuid(50) submit/wait 실패,
// 5=Setuid(50)이 None이 아님(root 특권 실패 - 회귀), 6=Setuid(99)
// 화이트리스트 등록 전인데 PermissionDenied가 아님(권한 상승 버그),
// 7=GrantSudoPermission 실패, 9=Setuid(99) 재시도 submit/wait 실패,
// 10=Setuid(99) 재시도가 None이 아님(화이트리스트 조회 경로 실패).
#include "libmc/authmgr.h"
#include "libmc/channel.h"
#include "libmc/process.h"
#include "libmc/syscall.h"

namespace {

constexpr char kAuthmgrChannelName[] = "authmgr";
constexpr mc::uint32_t kAuthmgrChannelNameLen = sizeof(kAuthmgrChannelName) - 1;
// sudotest가 authmgr보다 먼저 뜰 수 있다(다른 test 클라이언트들과
// 동일한 이유의 폴링 재시도).
constexpr mc::uint32_t kMaxConnectAttempts = 200000;
constexpr mc::uint32_t kCallerUid = 50;
constexpr mc::uint32_t kTargetUid = 99;
constexpr char kCallerLoginName[] = "sudocaller";
constexpr char kTargetLoginName[] = "sudotarget";

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

mc::BridgeHandle kConnectAuthmgr() {
    mc::ConnectChannelArgs connectArgs;
    for (mc::uint32_t attempt = 0; attempt < kMaxConnectAttempts; ++attempt) {
        connectArgs = mc::ConnectChannelArgs{};
        connectArgs.name = kAuthmgrChannelName;
        connectArgs.nameLength = kAuthmgrChannelNameLen;
        mc::SyscallToken token = mc::submit(mc::kSyscallEndpointConnectChannel, &connectArgs);
        if (token == 0 || !mc::wait(token)) {
            kFinish(1);
        }
        if (connectArgs.error == mc::ChannelError::None) {
            return connectArgs.bridge;
        }
        if (connectArgs.error != mc::ChannelError::NotFound) {
            kFinish(1);
        }
    }
    kFinish(1);
}

bool kCreateUser(mc::BridgeHandle bridge, mc::uint32_t uid, const char* loginName, mc::uint32_t loginNameLen) {
    mc::uint8_t sendBuf[mc::kAuthmgrMaxMessageBytes];
    auto* req = reinterpret_cast<mc::AuthmgrRequestHeader*>(sendBuf);
    *req = mc::AuthmgrRequestHeader{};
    req->requestType = mc::AuthmgrRequestType::CreateUser;
    auto* record = reinterpret_cast<mc::AuthmgrUserRecord*>(sendBuf + sizeof(mc::AuthmgrRequestHeader));
    *record = mc::AuthmgrUserRecord{};
    record->uid = uid;
    record->parentUid = 0;
    record->gid = uid;
    for (mc::uint32_t i = 0; i < loginNameLen; ++i) {
        record->loginName[i] = loginName[i];
    }
    const mc::uint32_t totalLen = sizeof(mc::AuthmgrRequestHeader) + sizeof(mc::AuthmgrUserRecord);
    req->header.totalLength = totalLen;
    req->header.frameKind = mc::AuthmgrFrameKind::Request;

    mc::ChannelWriteArgs writeArgs;
    writeArgs.bridge = bridge;
    writeArgs.data = sendBuf;
    writeArgs.length = totalLen;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointChannelWrite, &writeArgs);
    if (token == 0 || !mc::wait(token) || writeArgs.error != mc::ChannelError::None || writeArgs.bytesWritten != totalLen) {
        return false;
    }

    mc::uint8_t recvBuf[mc::kAuthmgrMaxMessageBytes];
    mc::ChannelReadArgs readArgs;
    readArgs.bridge = bridge;
    readArgs.buffer = recvBuf;
    readArgs.maxLength = sizeof(recvBuf);
    token = mc::submit(mc::kSyscallEndpointChannelRead, &readArgs);
    if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None ||
        readArgs.bytesRead < sizeof(mc::AuthmgrResponseHeader)) {
        return false;
    }
    auto* resp = reinterpret_cast<mc::AuthmgrResponseHeader*>(recvBuf);
    return resp->requestType == mc::AuthmgrRequestType::CreateUser && resp->error == 0;
}

bool kGrantSudo(mc::BridgeHandle bridge, mc::uint32_t callerUid, mc::uint32_t targetUid) {
    mc::uint8_t sendBuf[mc::kAuthmgrMaxMessageBytes];
    auto* req = reinterpret_cast<mc::AuthmgrRequestHeader*>(sendBuf);
    *req = mc::AuthmgrRequestHeader{};
    req->requestType = mc::AuthmgrRequestType::GrantSudoPermission;
    auto* body = reinterpret_cast<mc::AuthmgrSudoPermissionRequestBody*>(sendBuf + sizeof(mc::AuthmgrRequestHeader));
    *body = mc::AuthmgrSudoPermissionRequestBody{};
    body->callerUid = callerUid;
    body->targetUid = targetUid;
    const mc::uint32_t totalLen = sizeof(mc::AuthmgrRequestHeader) + sizeof(mc::AuthmgrSudoPermissionRequestBody);
    req->header.totalLength = totalLen;
    req->header.frameKind = mc::AuthmgrFrameKind::Request;

    mc::ChannelWriteArgs writeArgs;
    writeArgs.bridge = bridge;
    writeArgs.data = sendBuf;
    writeArgs.length = totalLen;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointChannelWrite, &writeArgs);
    if (token == 0 || !mc::wait(token) || writeArgs.error != mc::ChannelError::None || writeArgs.bytesWritten != totalLen) {
        return false;
    }

    mc::uint8_t recvBuf[mc::kAuthmgrMaxMessageBytes];
    mc::ChannelReadArgs readArgs;
    readArgs.bridge = bridge;
    readArgs.buffer = recvBuf;
    readArgs.maxLength = sizeof(recvBuf);
    token = mc::submit(mc::kSyscallEndpointChannelRead, &readArgs);
    if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None ||
        readArgs.bytesRead < sizeof(mc::AuthmgrResponseHeader)) {
        return false;
    }
    auto* resp = reinterpret_cast<mc::AuthmgrResponseHeader*>(recvBuf);
    return resp->requestType == mc::AuthmgrRequestType::GrantSudoPermission && resp->error == 0;
}

void kCloseBridge(mc::BridgeHandle bridge) {
    mc::CloseBridgeArgs closeArgs;
    closeArgs.bridge = bridge;
    mc::SyscallToken closeToken = mc::submit(mc::kSyscallEndpointCloseBridge, &closeArgs);
    if (closeToken != 0) {
        mc::wait(closeToken);
    }
}

}  // namespace

extern "C" void _start() {
    mc::BridgeHandle bridge = kConnectAuthmgr();
    if (!kCreateUser(bridge, kCallerUid, kCallerLoginName, sizeof(kCallerLoginName) - 1)) {
        kFinish(2);
    }
    if (!kCreateUser(bridge, kTargetUid, kTargetLoginName, sizeof(kTargetLoginName) - 1)) {
        kFinish(3);
    }
    kCloseBridge(bridge);

    // Setuid(50) - 이 프로세스는 root로 시작하므로 root 특권으로 즉시
    // 성공해야 한다(캐시 미스->authmgr 조회->root 분기).
    mc::SetuidArgs setuidArgs;
    setuidArgs.targetUid = kCallerUid;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSetuid, &setuidArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(4);
    }
    if (setuidArgs.error != mc::ChannelError::None) {
        kFinish(5);
    }

    // 이제 uid=50(형제 uid 99에 대해 root도 조상도 아님) - 화이트리스트
    // 등록 전이므로 반드시 PermissionDenied여야 한다(권한 상승 회귀
    // 검사 - 이게 성공하면 심각한 버그).
    setuidArgs = mc::SetuidArgs{};
    setuidArgs.targetUid = kTargetUid;
    token = mc::submit(mc::kSyscallEndpointSetuid, &setuidArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(6);
    }
    if (setuidArgs.error != mc::ChannelError::PermissionDenied) {
        kFinish(6);
    }

    // authmgr에 (50->99) 화이트리스트 등록(이전 브리지는 이미 닫았으니
    // 새로 연결).
    bridge = kConnectAuthmgr();
    if (!kGrantSudo(bridge, kCallerUid, kTargetUid)) {
        kFinish(7);
    }
    kCloseBridge(bridge);

    // 재시도 - 이번엔 kSetuidOnExecImpl의 sudo 화이트리스트 조회
    // 경로를 타고 성공해야 한다.
    setuidArgs = mc::SetuidArgs{};
    setuidArgs.targetUid = kTargetUid;
    token = mc::submit(mc::kSyscallEndpointSetuid, &setuidArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(9);
    }
    if (setuidArgs.error != mc::ChannelError::None) {
        kFinish(10);
    }

    kFinish(0);
}
