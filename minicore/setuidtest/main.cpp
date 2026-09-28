// minicore/setuidtest - DC-90A66932 (A) 채택으로 새로 배선한
// kSetuidOnExecImpl(user_record.cpp)의 authmgr 캐시미스 왕복 E2E 검증
// 전용 클라이언트. authtest와 동일한 이유의 out-of-tree
// add_subdirectory 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP
// kmain.cpp 훅으로만 자동 실행).
//
// 시나리오: (1) authmgr에 아직 존재하지 않는 uid(kTestUid)를
// CreateUser로 새로 만든다 - 이 시점에 커널의 UserRecordCache에는
// 당연히 없다(부팅 시 root 하나만 심어져 있음, user_record.cpp
// UserRecordCache::init() 참고). (2) 이 프로세스는 init의 자손이라
// uid=0(root)으로 시작하므로(process.h Process::uid 기본값), 그 uid로
// Setuid(kTestUid) syscall을 호출한다 - kSetuid()가 캐시 미스로
// ServiceUnavailable을 내부적으로 만나 kSetuidOnExecImpl이
// authmgr_client.h 경로(kAuthmgrBeginConnect/WriteLookupRequest/
// ReadLookupResponse)로 authmgr에 직접 질의해 캐시를 채운 뒤 재시도
// 하는 새 코드 경로를 반드시 거쳐야만 성공할 수 있다(이 배선이
// 없었다면 예전처럼 영구히 ServiceUnavailable). 즉 이 syscall의
// error==None 하나만으로 새 경로 전체(커널 전용 Channel 클라이언트
// 연결 수립 + 쓰기 + 읽기 + 캐시 삽입 + kSetuid 재시도)가 실제로
// 동작했다는 증거가 된다.
//
// exitCode: 0=전 구간 성공, 1=Connect 실패, 2=CreateUser Write 실패,
// 3=CreateUser Read 실패, 4=CreateUser 응답 에러, 5=Setuid submit/wait
// 실패, 6=Setuid 응답 에러(캐시미스 왕복 실패 - authmgr_client 경로가
// 깨졌다는 뜻).
#include "libmc/authmgr.h"
#include "libmc/channel.h"
#include "libmc/process.h"
#include "libmc/syscall.h"

namespace {

constexpr char kAuthmgrChannelName[] = "authmgr";
constexpr mc::uint32_t kAuthmgrChannelNameLen = sizeof(kAuthmgrChannelName) - 1;
// setuidtest가 authmgr보다 먼저 뜰 수 있다(authtest/sockclient와 동일한
// 이유의 폴링 재시도).
constexpr mc::uint32_t kMaxConnectAttempts = 200000;
// authtest가 이미 uid=42를 씀(kTestUid) - 겹치면 CreateUser가
// AlreadyExists류로 실패할 수 있으니 다른 값을 쓴다.
constexpr mc::uint32_t kTestUid = 77;
constexpr char kTestLoginName[] = "setuidcachemiss";

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

    // ---- CreateUser(uid=77, parentUid=0, loginName="setuidcachemiss") ----
    mc::uint8_t sendBuf[mc::kAuthmgrMaxMessageBytes];
    auto* createReq = reinterpret_cast<mc::AuthmgrRequestHeader*>(sendBuf);
    *createReq = mc::AuthmgrRequestHeader{};
    createReq->requestType = mc::AuthmgrRequestType::CreateUser;
    auto* newRecord = reinterpret_cast<mc::AuthmgrUserRecord*>(sendBuf + sizeof(mc::AuthmgrRequestHeader));
    *newRecord = mc::AuthmgrUserRecord{};
    newRecord->uid = kTestUid;
    newRecord->parentUid = 0;  // root의 직계 자손 - root는 어차피 임의 uid로 전환 가능하니 무관하지만 값 자체는 채워둔다.
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

    mc::CloseBridgeArgs closeArgs;
    closeArgs.bridge = bridge;
    token = mc::submit(mc::kSyscallEndpointCloseBridge, &closeArgs);
    if (token != 0) {
        mc::wait(token);
    }

    // ---- Setuid(77) - 이 시점 커널 UserRecordCache는 kTestUid를 전혀
    // 모른다(방금 authmgr에만 만들었다) - kSetuidOnExecImpl의 authmgr
    // 캐시미스 왕복 경로를 강제로 타게 만드는 핵심 스텝. ----
    mc::SetuidArgs setuidArgs;
    setuidArgs.targetUid = kTestUid;
    token = mc::submit(mc::kSyscallEndpointSetuid, &setuidArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(5);
    }
    if (setuidArgs.error != mc::ChannelError::None) {
        kFinish(6);
    }

    kFinish(0);
}
