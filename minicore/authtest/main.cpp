// minicore/authtest - authmgr(PN-24A2B6F5/PN-B6DB692C) libkvdb 연동
// (CreateUser/LookupByUid) E2E 검증 전용 클라이언트. sockclient와 동일한
// 이유의 out-of-tree add_subdirectory 관례 - 부팅 매니페스트에는
// 포함하지 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
//
// commit 4aba7cc(2026-09-23)가 이 왕복(CreateUser->LookupByUid)을
// pubreg를 임시 클라이언트 삼아 시도했으나 응답이 끝내 오지 않았다
// (크래시 없음, 원인 미확정) - 그 시점은 DC-54D69BEE/DC-2CB9DDA0의
// AsyncReactor/스케줄러 핫패스 캐스케이딩 NMI watchdog 계열이 아직
// 전혀 해소되지 않은 때였다(오늘 DC-2CB9DDA0 방향(1) 감사로 dbgdriver
// 60/60 무재현까지 완전히 해소됨). 이 프로그램은 그 원래 실패한 왕복을
// 전용 영구 테스트 클라이언트로 다시 시도해, 그 캐스케이딩 문제가
// 실제로 이 증상의 원인이었는지(사라졌으면) 아니면 별개 원인이
// 남아있는지(여전히 실패하면) 가른다.
//
// exitCode: 0=전 구간 성공(CreateUser+LookupByUid 왕복+필드 일치),
// 1=Connect 실패, 2=CreateUser Write 실패, 3=CreateUser Read 실패,
// 4=CreateUser 응답 에러, 5=LookupByUid Write 실패, 6=LookupByUid
// Read 실패, 7=LookupByUid 응답 에러, 8=uid/gid 필드 불일치,
// 9=loginName 필드 불일치.
#include "libmc/authmgr.h"
#include "libmc/channel.h"
#include "libmc/syscall.h"

namespace {

constexpr char kAuthmgrChannelName[] = "authmgr";
constexpr mc::uint32_t kAuthmgrChannelNameLen = sizeof(kAuthmgrChannelName) - 1;
// authtest가 authmgr보다 먼저 뜰 수 있다(init/pubreg처럼 조정 채널이
// 없음 - sockclient/socktest와 동일한 이유의 폴링 재시도).
constexpr mc::uint32_t kMaxConnectAttempts = 200000;
constexpr mc::uint32_t kTestUid = 42;
constexpr char kTestLoginName[] = "testuser";

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

}  // namespace

extern "C" void _start() {
    mc::ConnectChannelArgs connectArgs;
    bool connected = false;
    for (mc::uint32_t attempt = 0; attempt < kMaxConnectAttempts; ++attempt) {
        connectArgs = mc::ConnectChannelArgs{};
        connectArgs.name = kAuthmgrChannelName;
        connectArgs.nameLength = kAuthmgrChannelNameLen;
        mc::SyscallToken token = mc::submit(mc::kSyscallEndpointConnectChannel, &connectArgs);
        if (token == 0 || !mc::wait(token)) {
            kFinish(1);
        }
        if (connectArgs.error == mc::ChannelError::None) {
            connected = true;
            break;
        }
        if (connectArgs.error != mc::ChannelError::NotFound) {
            kFinish(1);
        }
    }
    if (!connected) {
        kFinish(1);
    }
    const mc::BridgeHandle bridge = connectArgs.bridge;

    // ---- CreateUser(uid=42, loginName="testuser") ----
    mc::uint8_t sendBuf[mc::kAuthmgrMaxMessageBytes];
    auto* createReq = reinterpret_cast<mc::AuthmgrRequestHeader*>(sendBuf);
    *createReq = mc::AuthmgrRequestHeader{};
    createReq->requestType = mc::AuthmgrRequestType::CreateUser;
    auto* newRecord = reinterpret_cast<mc::AuthmgrUserRecord*>(sendBuf + sizeof(mc::AuthmgrRequestHeader));
    *newRecord = mc::AuthmgrUserRecord{};
    newRecord->uid = kTestUid;
    newRecord->parentUid = 0;
    newRecord->gid = kTestUid;
    for (mc::uint32_t i = 0; i < sizeof(kTestLoginName) - 1; ++i) {
        newRecord->loginName[i] = kTestLoginName[i];
    }
    const mc::uint32_t createTotalLen = sizeof(mc::AuthmgrRequestHeader) + sizeof(mc::AuthmgrUserRecord);
    createReq->header.totalLength = createTotalLen;
    createReq->header.frameKind = mc::AuthmgrFrameKind::Request;

    mc::ChannelWriteArgs writeArgs;
    writeArgs.bridge = bridge;
    writeArgs.data = sendBuf;
    writeArgs.length = createTotalLen;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointChannelWrite, &writeArgs);
    if (token == 0 || !mc::wait(token) || writeArgs.error != mc::ChannelError::None ||
        writeArgs.bytesWritten != createTotalLen) {
        kFinish(2);
    }

    mc::uint8_t recvBuf[mc::kAuthmgrMaxMessageBytes];
    mc::ChannelReadArgs readArgs;
    readArgs.bridge = bridge;
    readArgs.buffer = recvBuf;
    readArgs.maxLength = sizeof(recvBuf);
    token = mc::submit(mc::kSyscallEndpointChannelRead, &readArgs);
    if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None ||
        readArgs.bytesRead < sizeof(mc::AuthmgrResponseHeader)) {
        kFinish(3);
    }
    auto* createResp = reinterpret_cast<mc::AuthmgrResponseHeader*>(recvBuf);
    if (createResp->requestType != mc::AuthmgrRequestType::CreateUser || createResp->error != 0) {
        kFinish(4);
    }

    // ---- LookupByUid(42) ----
    auto* lookupReq = reinterpret_cast<mc::AuthmgrRequestHeader*>(sendBuf);
    *lookupReq = mc::AuthmgrRequestHeader{};
    lookupReq->requestType = mc::AuthmgrRequestType::LookupByUid;
    auto* lookupBody =
        reinterpret_cast<mc::AuthmgrLookupByUidRequestBody*>(sendBuf + sizeof(mc::AuthmgrRequestHeader));
    *lookupBody = mc::AuthmgrLookupByUidRequestBody{};
    lookupBody->uid = kTestUid;
    const mc::uint32_t lookupTotalLen = sizeof(mc::AuthmgrRequestHeader) + sizeof(mc::AuthmgrLookupByUidRequestBody);
    lookupReq->header.totalLength = lookupTotalLen;
    lookupReq->header.frameKind = mc::AuthmgrFrameKind::Request;

    writeArgs = mc::ChannelWriteArgs{};
    writeArgs.bridge = bridge;
    writeArgs.data = sendBuf;
    writeArgs.length = lookupTotalLen;
    token = mc::submit(mc::kSyscallEndpointChannelWrite, &writeArgs);
    if (token == 0 || !mc::wait(token) || writeArgs.error != mc::ChannelError::None ||
        writeArgs.bytesWritten != lookupTotalLen) {
        kFinish(5);
    }

    readArgs = mc::ChannelReadArgs{};
    readArgs.bridge = bridge;
    readArgs.buffer = recvBuf;
    readArgs.maxLength = sizeof(recvBuf);
    token = mc::submit(mc::kSyscallEndpointChannelRead, &readArgs);
    if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None ||
        readArgs.bytesRead < sizeof(mc::AuthmgrResponseHeader) + sizeof(mc::AuthmgrUserRecord)) {
        kFinish(6);
    }
    auto* lookupResp = reinterpret_cast<mc::AuthmgrResponseHeader*>(recvBuf);
    if (lookupResp->requestType != mc::AuthmgrRequestType::LookupByUid || lookupResp->error != 0) {
        kFinish(7);
    }
    auto* foundRecord = reinterpret_cast<mc::AuthmgrUserRecord*>(recvBuf + sizeof(mc::AuthmgrResponseHeader));
    if (foundRecord->uid != kTestUid || foundRecord->gid != kTestUid) {
        kFinish(8);
    }
    for (mc::uint32_t i = 0; i < sizeof(kTestLoginName) - 1; ++i) {
        if (foundRecord->loginName[i] != kTestLoginName[i]) {
            kFinish(9);
        }
    }

    mc::CloseBridgeArgs closeArgs;
    closeArgs.bridge = bridge;
    token = mc::submit(mc::kSyscallEndpointCloseBridge, &closeArgs);
    if (token != 0) {
        mc::wait(token);
    }

    kFinish(0);
}
