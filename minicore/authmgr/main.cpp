// minicore/authmgr: 사용자 신원 관리(authmgr) 서비스(6번째 커널
// 서비스, SP-8B6B8D25 §3.1 항목8/SP-30FCC8AE §1-D, PN-24A2B6F5/
// PN-CFEAEF40/PN-BDEAA9B5) - devmgr/init/pubreg와 같은 이유로 "libmc를
// 통해서만 커널에 요청한다"는 모양부터 갖춰 둔다(minicore/pubreg/
// main.cpp와 완전히 동일한 관례 - 멀티플렉싱 accept+read/write 상태
// 기계도 그대로 재사용).
//
// **[갱신, 2026-09-23, PN-24A2B6F5/PN-B6DB692C] libkvdb 연동 시작** -
// `LookupByUid`/`CreateUser` 요청 2종을 실제 `kvdb::Store`로 처리한다.
// **CreateUser는 아직 아무 권한 검사도 하지 않는다**(caller uid를
// 와이어로 전달하는 규약 자체가 미정 - `libmc/authmgr.h`의
// `AuthmgrRequestType::CreateUser` 문서 주석 참고) - 지금은 이
// 서비스 자신의 libkvdb 배선을 검증하기 위한 내부용/테스트 전용
// 요청이고, 어떤 kernel syscall도 아직 이 요청을 트리거하지 않는다.
// sudo/su 판정, libkproto로의 추출은 여전히 후속 세션 몫.
#include "libkvdb/kvdb.h"
#include "libmc/authmgr.h"
#include "libmc/channel.h"
#include "libmc/syscall.h"

namespace {

constexpr char kAuthmgrChannelName[] = "authmgr";

// [신규, 2026-09-23, PN-24A2B6F5 §1-C/D 항목1/항목2] uid(4바이트
// 리틀엔디안 그대로)->AuthmgrUserRecord 저장소가 권위 있는 저장소,
// loginName->uid(4바이트)는 별도 인스턴스(항목2가 확정한 "완전히
// 분리된 두 번째 Store" 방침 그대로) - v1은 메모리 전용(재부팅 시
// 소실, fs 생기기 전까지는 설계 대상도 아님, PN-24A2B6F5 항목4).
// 용량은 커널 UserRecordCache와 동일한 1024(kernel::
// kUserRecordMaxCacheEntries와 값만 일치, 실제 상수 공유는 커널/
// 유저 경계 때문에 불가능 - 어긋나면 캐시보다 authmgr 저장소가
// 먼저 꽉 차는 비대칭이 생기니 착수 세션이 값을 바꿀 땐 항상 같이
// 바꿀 것).
constexpr mc::uint32_t kMaxUserRecords = 1024;
kvdb::Store<kMaxUserRecords, sizeof(mc::uint32_t), sizeof(mc::AuthmgrUserRecord)> gUserRecordsByUid;
kvdb::Store<kMaxUserRecords, mc::kAuthmgrLoginNameMaxBytes, sizeof(mc::uint32_t)> gUidByLoginName;

// [설계 확정, 2026-09-17] root(uid=0)는 authmgr 가용성과 무관하게
// 항상 성립해야 하므로 커널도 부팅 시 하드코딩한다(user_record.cpp
// UserRecordCache::init()) - authmgr도 자신의 저장소에 같은 값을
// 미리 심어 둬, 나중에 커널이 실제로 authmgr에 root를 질의하게 되면
// (지금은 아직 그 경로가 없음) 같은 값을 돌려줄 수 있게 한다.
void kSeedRootUserRecord() {
    mc::AuthmgrUserRecord root;
    root.uid = 0;
    root.parentUid = 0;
    root.gid = 0;
    const unsigned char key[sizeof(mc::uint32_t)] = {0, 0, 0, 0};
    gUserRecordsByUid.put(key, sizeof(key), reinterpret_cast<const unsigned char*>(&root), sizeof(root));
}

// v1 상한(RM-23F4B687 §4 - 실측 후 조정) - pubreg의 kMaxConnections
// 보다 훨씬 작게 잡는다(v1은 Ping 왕복 하나뿐이라 동시 접속 부담이
// pubreg만큼 크지 않다고 판단 - 실사용 패턴이 드러나면 재검토).
constexpr mc::uint32_t kMaxConnections = 8;

enum class ConnState : mc::uint8_t { Reading, Writing };

struct Connection {
    bool inUse = false;
    ConnState state = ConnState::Reading;
    mc::BridgeHandle bridge = 0;
    mc::SyscallToken token = 0;  // 이 연결의 현재 유일한 미해결 토큰(읽기 또는 쓰기)
    mc::uint32_t bytesBuffered = 0;  // buf 안에 이미 쌓인 바이트 수(부분 메시지 재조립용)
    mc::ChannelReadArgs readArgs;
    mc::ChannelWriteArgs writeArgs;
    mc::uint8_t buf[mc::kAuthmgrMaxMessageBytes];
};

Connection gConnections[kMaxConnections];

mc::BridgeHandle gServerChannel = 0;
mc::AcceptFromChannelArgs gAcceptArgs;
mc::SyscallToken gAcceptToken = 0;

void kSubmitAccept() {
    gAcceptArgs = mc::AcceptFromChannelArgs{};
    gAcceptArgs.channelHandle = gServerChannel;
    gAcceptToken = mc::submit(mc::kSyscallEndpointAcceptFromChannel, &gAcceptArgs);
}

void kSubmitRead(Connection* conn) {
    conn->state = ConnState::Reading;
    conn->readArgs = mc::ChannelReadArgs{};
    conn->readArgs.bridge = conn->bridge;
    conn->readArgs.buffer = conn->buf + conn->bytesBuffered;
    conn->readArgs.maxLength = sizeof(conn->buf) - conn->bytesBuffered;
    conn->token = mc::submit(mc::kSyscallEndpointChannelRead, &conn->readArgs);
}

void kSubmitWrite(Connection* conn, mc::uint32_t length) {
    conn->state = ConnState::Writing;
    conn->writeArgs = mc::ChannelWriteArgs{};
    conn->writeArgs.bridge = conn->bridge;
    conn->writeArgs.data = conn->buf;
    conn->writeArgs.length = length;
    conn->token = mc::submit(mc::kSyscallEndpointChannelWrite, &conn->writeArgs);
}

void kCloseConnection(Connection* conn) {
    mc::CloseBridgeArgs closeArgs;
    closeArgs.bridge = conn->bridge;
    mc::SyscallToken closeToken = mc::submit(mc::kSyscallEndpointCloseBridge, &closeArgs);
    if (closeToken != 0) {
        mc::wait(closeToken);
    }
    *conn = Connection{};
}

void kUidKey(mc::uint32_t uid, unsigned char out[sizeof(mc::uint32_t)]) {
    const auto* src = reinterpret_cast<const unsigned char*>(&uid);
    for (mc::uint32_t i = 0; i < sizeof(mc::uint32_t); ++i) {
        out[i] = src[i];
    }
}

// Request 프레임 하나를 처리해 conn->buf 맨 앞에 Response(고정
// 헤더 + requestType별 가변 본문)를 채우고 그 전체 길이를 반환한다
// (pubreg의 kHandleRegister/kHandleQuery와 동일한 관례). 알 수 없는
// requestType/본문 길이 부족은 전부 NotSupported 에러 응답(연결을
// 끊지 않고 계속 받는다 - 알 수 없는 요청 하나가 연결 전체를 죽일
// 이유는 없음).
mc::uint32_t kHandleRequest(Connection* conn, const mc::uint8_t* body, mc::uint32_t bodyLen) {
    mc::AuthmgrRequestHeader req{};
    constexpr mc::uint32_t kFixedHeaderLen = sizeof(mc::AuthmgrRequestHeader) - sizeof(mc::AuthmgrMessageHeader);
    if (bodyLen < kFixedHeaderLen) {
        return 0;
    }
    mc::uint8_t* dst = reinterpret_cast<mc::uint8_t*>(&req) + sizeof(mc::AuthmgrMessageHeader);
    for (mc::uint32_t i = 0; i < kFixedHeaderLen; ++i) {
        dst[i] = body[i];
    }
    const mc::uint8_t* variableBody = body + kFixedHeaderLen;
    const mc::uint32_t variableBodyLen = bodyLen - kFixedHeaderLen;

    auto* resp = reinterpret_cast<mc::AuthmgrResponseHeader*>(conn->buf);
    resp->requestType = req.requestType;
    mc::uint32_t responseBodyLen = 0;
    mc::uint8_t* responseBody = conn->buf + sizeof(mc::AuthmgrResponseHeader);

    switch (req.requestType) {
        case mc::AuthmgrRequestType::Ping:
            resp->error = 0;  // mc::ChannelError::None - Pong은 이 응답 자체(본문 없음)
            break;

        case mc::AuthmgrRequestType::LookupByUid: {
            if (variableBodyLen < sizeof(mc::AuthmgrLookupByUidRequestBody)) {
                return 0;
            }
            const auto* reqBody = reinterpret_cast<const mc::AuthmgrLookupByUidRequestBody*>(variableBody);
            unsigned char key[sizeof(mc::uint32_t)];
            kUidKey(reqBody->uid, key);
            mc::AuthmgrUserRecord record;
            unsigned int outLen = 0;
            kvdb::ErrorCode kvErr = gUserRecordsByUid.get(key, sizeof(key), reinterpret_cast<unsigned char*>(&record),
                                                            sizeof(record), &outLen);
            if (kvErr == kvdb::ErrorCode::None && outLen == sizeof(record)) {
                resp->error = 0;  // None
                mc::uint8_t* src = reinterpret_cast<mc::uint8_t*>(&record);
                for (mc::uint32_t i = 0; i < sizeof(record); ++i) {
                    responseBody[i] = src[i];
                }
                responseBodyLen = sizeof(record);
            } else {
                resp->error = static_cast<mc::uint32_t>(mc::ChannelError::NotFound);
            }
            break;
        }

        case mc::AuthmgrRequestType::CreateUser: {
            // [알려진 제약, libmc/authmgr.h 문서 주석 참고] 권한 검사
            // 없음 - 아직 이 요청을 트리거하는 kernel syscall 경로
            // 자체가 없어 실사용 위험이 없다.
            if (variableBodyLen < sizeof(mc::AuthmgrUserRecord)) {
                return 0;
            }
            const auto* newRecord = reinterpret_cast<const mc::AuthmgrUserRecord*>(variableBody);
            unsigned char key[sizeof(mc::uint32_t)];
            kUidKey(newRecord->uid, key);
            if (gUserRecordsByUid.contains(key, sizeof(key))) {
                resp->error = static_cast<mc::uint32_t>(mc::ChannelError::AlreadyExists);
                break;
            }
            kvdb::ErrorCode kvErr = gUserRecordsByUid.put(
                key, sizeof(key), reinterpret_cast<const unsigned char*>(newRecord), sizeof(*newRecord));
            if (kvErr != kvdb::ErrorCode::None) {
                resp->error = static_cast<mc::uint32_t>(mc::ChannelError::ResourceExhausted);
                break;
            }
            // 로그인명 인덱스도 함께 갱신(PN-24A2B6F5 항목2 방침 -
            // 완전히 분리된 두 번째 Store) - 이 인덱스를 실제로 읽는
            // 조회 요청은 아직 없다(소비자가 생기면 그때 추가).
            mc::uint32_t loginNameLen = 0;
            while (loginNameLen < mc::kAuthmgrLoginNameMaxBytes && newRecord->loginName[loginNameLen] != '\0') {
                ++loginNameLen;
            }
            if (loginNameLen > 0) {
                gUidByLoginName.put(reinterpret_cast<const unsigned char*>(newRecord->loginName), loginNameLen,
                                     reinterpret_cast<const unsigned char*>(&newRecord->uid),
                                     sizeof(newRecord->uid));
            }
            resp->error = 0;  // None
            break;
        }

        default:
            resp->error = static_cast<mc::uint32_t>(mc::ChannelError::NotSupported);
            break;
    }

    resp->header.frameKind = mc::AuthmgrFrameKind::Response;
    resp->header.totalLength = sizeof(mc::AuthmgrResponseHeader) + responseBodyLen;
    return resp->header.totalLength;
}

// conn->buf[0..bytesBuffered)에 완전한 메시지가 쌓여 있으면 처리하고
// true를 반환한다 - pubreg의 kTryHandleOneMessage와 동일한 관례
// (v1은 한 메시지 처리당 버퍼를 완전히 비우는 단순 모델).
bool kTryHandleOneMessage(Connection* conn, mc::uint32_t* outResponseLength) {
    if (conn->bytesBuffered < sizeof(mc::AuthmgrMessageHeader)) {
        return false;
    }
    const auto* header = reinterpret_cast<const mc::AuthmgrMessageHeader*>(conn->buf);
    const mc::uint32_t totalLength = header->totalLength;
    if (totalLength < sizeof(mc::AuthmgrMessageHeader) || totalLength > sizeof(conn->buf) ||
        conn->bytesBuffered < totalLength) {
        return false;
    }

    const mc::uint8_t* body = conn->buf + sizeof(mc::AuthmgrMessageHeader);
    const mc::uint32_t bodyLen = totalLength - sizeof(mc::AuthmgrMessageHeader);
    mc::uint32_t responseLength = 0;
    if (header->frameKind == mc::AuthmgrFrameKind::Request) {
        responseLength = kHandleRequest(conn, body, bodyLen);
    }
    // Response/Notification 프레임이 클라이언트에서 도착하는 건 v1
    // 프로토콜상 있을 수 없는 일(둘 다 서버->클라이언트 전용) - 응답
    // 없이 그냥 무시(pubreg의 알 수 없는 opcode 처리와 동일한 관례).

    conn->bytesBuffered = 0;
    *outResponseLength = responseLength;
    return true;
}

}  // namespace

extern "C" void _start() {
    kSeedRootUserRecord();

    mc::OpenChannelArgs openArgs;
    openArgs.name = kAuthmgrChannelName;
    openArgs.nameLength = sizeof(kAuthmgrChannelName) - 1;

    mc::SyscallToken openToken = mc::submit(mc::kSyscallEndpointOpenChannel, &openArgs);
    if (openToken == 0 || !mc::wait(openToken) || openArgs.error != mc::ChannelError::None) {
        // 이름 충돌(이미 다른 authmgr 인스턴스가 떠 있음) 또는 자원
        // 고갈 - 이 서비스는 계속 존재할 이유가 없으므로 종료한다
        // (pubreg의 동일 실패 처리와 같은 이유).
        mc::selfTerminate(1);
    }
    gServerChannel = openArgs.channelHandle;
    kSubmitAccept();

    for (;;) {
        mc::SyscallToken tokens[1 + kMaxConnections];
        mc::uint32_t tokenCount = 0;
        tokens[tokenCount++] = gAcceptToken;
        for (mc::uint32_t i = 0; i < kMaxConnections; ++i) {
            if (gConnections[i].inUse) {
                tokens[tokenCount++] = gConnections[i].token;
            }
        }

        mc::WaitAnyOfSyscallArgs result = mc::waitAnyForMultipleSyscall(tokens, tokenCount);
        if (result.resultOutcome == mc::MultiWaitOutcome::Invalid) {
            // 토큰 배열 자체가 잘못됐거나(있을 수 없음) 커널 트랩이
            // 실패했다 - 복구 불가능한 상태로 보고 서비스를 끝낸다.
            break;
        }

        if (result.resultToken == gAcceptToken) {
            if (result.resultOutcome == mc::MultiWaitOutcome::Completed && openArgs.error == mc::ChannelError::None &&
                gAcceptArgs.error == mc::ChannelError::None) {
                bool accepted = false;
                for (mc::uint32_t i = 0; i < kMaxConnections; ++i) {
                    if (!gConnections[i].inUse) {
                        gConnections[i] = Connection{};
                        gConnections[i].inUse = true;
                        gConnections[i].bridge = gAcceptArgs.bridge;
                        kSubmitRead(&gConnections[i]);
                        accepted = true;
                        break;
                    }
                }
                if (!accepted) {
                    // v1 상한 초과 - 더 받을 자리가 없어 그냥 닫는다.
                    mc::CloseBridgeArgs closeArgs;
                    closeArgs.bridge = gAcceptArgs.bridge;
                    mc::SyscallToken closeToken = mc::submit(mc::kSyscallEndpointCloseBridge, &closeArgs);
                    if (closeToken != 0) {
                        mc::wait(closeToken);
                    }
                }
            } else if (gAcceptArgs.error != mc::ChannelError::None) {
                // 채널이 소멸됐거나(NotFound) 복구 불가능 - 더 이상
                // accept할 수 없으므로 루프를 끝낸다.
                break;
            }
            kSubmitAccept();
            continue;
        }

        for (mc::uint32_t i = 0; i < kMaxConnections; ++i) {
            Connection* conn = &gConnections[i];
            if (!conn->inUse || conn->token != result.resultToken) {
                continue;
            }

            if (conn->state == ConnState::Reading) {
                if (result.resultOutcome != mc::MultiWaitOutcome::Completed ||
                    conn->readArgs.error != mc::ChannelError::None || conn->readArgs.bytesRead == 0) {
                    kCloseConnection(conn);
                    break;
                }
                conn->bytesBuffered += static_cast<mc::uint32_t>(conn->readArgs.bytesRead);
                mc::uint32_t responseLength = 0;
                if (kTryHandleOneMessage(conn, &responseLength)) {
                    if (responseLength > 0) {
                        kSubmitWrite(conn, responseLength);
                    } else {
                        // 처리 실패(포맷 오류) - 응답 없이 다음 메시지를
                        // 계속 기다린다(pubreg와 동일한 관례).
                        kSubmitRead(conn);
                    }
                } else {
                    // 아직 완전한 메시지가 안 모임 - 이어서 더 읽는다.
                    kSubmitRead(conn);
                }
            } else {  // ConnState::Writing
                if (result.resultOutcome != mc::MultiWaitOutcome::Completed ||
                    conn->writeArgs.error != mc::ChannelError::None) {
                    kCloseConnection(conn);
                    break;
                }
                kSubmitRead(conn);
            }
            break;
        }
    }

    mc::selfTerminate(0);
}
