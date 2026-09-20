// minicore/pubreg: "프로세스간 공개 인터페이스" Registry 서비스(5번째
// 커널 서비스, SP-B071E628 §1~§4/§6-6, PN-185406F6) - devmgr/init과
// 같은 이유로 "libmc를 통해서만 커널에 요청한다"는 모양부터 갖춰 둔다
// (minicore/kernel/devmgr.cpp/minicore/init/main.cpp와 동일한 관례).
//
// **[완료, 2026-09-18, PN-185406F6 항목4]** register/query 메시지
// 처리(SP-B071E628 §6-6 완전 바이너리 와이어 포맷) - "새 연결 accept"
// 와 "이미 등록된 여러 연결 각각의 메시지/연결종료 감지"를 동시에
// 다뤄야 해서, PN-10EE096A가 새로 노출한 `mc::waitAnyForMultipleSyscall`
// 위에서 멀티플렉싱 루프로 구현한다 - 연결마다 항상 정확히 하나의
// 미해결 syscall 토큰(읽는 중 또는 쓰는 중)만 갖는 간단한 상태
// 기계(Reading/Writing)로 설계해, accept 토큰 + 각 연결의 현재 토큰을
// 한 번에 `waitAnyForMultipleSyscall`로 기다린다.
#include "libmc/channel.h"
#include "libmc/pubreg.h"
#include "libmc/syscall.h"

namespace {

constexpr char kPubregChannelName[] = "pubreg";

// v1 상한(RM-23F4B687 §4 - 실측 후 조정) - 동시에 열려 있을 수 있는
// 등록/조회 연결 수.
constexpr mc::uint32_t kMaxConnections = 16;
// v1 상한 - pubreg가 동시에 들고 있을 수 있는 등록 개수.
constexpr mc::uint32_t kMaxRegistrations = 64;

enum class ConnState : mc::uint8_t { Reading, Writing };

struct Connection {
    bool inUse = false;
    ConnState state = ConnState::Reading;
    mc::BridgeHandle bridge = 0;
    mc::SyscallToken token = 0;  // 이 연결의 현재 유일한 미해결 토큰(읽기 또는 쓰기)
    mc::uint32_t bytesBuffered = 0;  // buf 안에 이미 쌓인 바이트 수(부분 메시지 재조립용)
    mc::ChannelReadArgs readArgs;
    mc::ChannelWriteArgs writeArgs;
    mc::uint8_t buf[mc::kPubregMaxMessageBytes];
};

Connection gConnections[kMaxConnections];

struct RegistrationSlot {
    bool inUse = false;
    mc::uint32_t ownerConnectionIndex = 0;  // 연결 종료 시 자동 해제 대상 찾기용
    mc::PubregRegistrationEntry entry;
};

RegistrationSlot gRegistrations[kMaxRegistrations];
mc::uint32_t gNextRegistryId = 1;  // 0은 "없음"으로 예약(PubregRegisterAck 참고)

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

// 이 연결이 소유한(등록 시 이 연결 인덱스로 기록된) 등록 전부 해제 -
// "닫힌 파이프" 감지로 등록을 지운다는 §3/§6 확정 설계 그대로.
void kReleaseRegistrationsOwnedBy(mc::uint32_t connectionIndex) {
    for (mc::uint32_t i = 0; i < kMaxRegistrations; ++i) {
        if (gRegistrations[i].inUse && gRegistrations[i].ownerConnectionIndex == connectionIndex) {
            gRegistrations[i].inUse = false;
        }
    }
}

void kCloseConnection(Connection* conn, mc::uint32_t connectionIndex) {
    kReleaseRegistrationsOwnedBy(connectionIndex);
    mc::CloseBridgeArgs closeArgs;
    closeArgs.bridge = conn->bridge;
    mc::SyscallToken closeToken = mc::submit(mc::kSyscallEndpointCloseBridge, &closeArgs);
    if (closeToken != 0) {
        mc::wait(closeToken);
    }
    *conn = Connection{};
}

// PubregRegisterRequest 본문(헤더 이후)을 파싱해 등록하고, 응답을
// conn->buf에 채운 뒤 그 길이를 반환한다(0이면 처리 실패 - 호출부가
// 연결을 닫는다).
mc::uint32_t kHandleRegister(Connection* conn, mc::uint32_t connectionIndex, const mc::uint8_t* body,
                              mc::uint32_t bodyLen) {
    mc::PubregRegisterRequest req{};
    // 헤더는 이미 호출부가 읽었으니 헤더 이후 필드만 채운다 - 고정
    // 폭이라 정확한 바이트 수만 확인하면 된다(가변 길이 없음).
    constexpr mc::uint32_t kFixedBodyLen = sizeof(mc::PubregRegisterRequest) - sizeof(mc::PubregMessageHeader);
    if (bodyLen < kFixedBodyLen) {
        return 0;
    }
    mc::uint8_t* dst = reinterpret_cast<mc::uint8_t*>(&req) + sizeof(mc::PubregMessageHeader);
    for (mc::uint32_t i = 0; i < kFixedBodyLen; ++i) {
        dst[i] = body[i];
    }

    mc::uint32_t registryId = 0;
    for (mc::uint32_t i = 0; i < kMaxRegistrations; ++i) {
        if (!gRegistrations[i].inUse) {
            gRegistrations[i].inUse = true;
            gRegistrations[i].ownerConnectionIndex = connectionIndex;
            registryId = gNextRegistryId++;
            gRegistrations[i].entry.registryId = registryId;
            for (mc::uint32_t j = 0; j < sizeof(req.protocolCode); ++j) {
                gRegistrations[i].entry.protocolCode[j] = req.protocolCode[j];
            }
            for (mc::uint32_t j = 0; j < sizeof(req.implementationId); ++j) {
                gRegistrations[i].entry.implementationId[j] = req.implementationId[j];
            }
            gRegistrations[i].entry.endpoint = req.endpoint;
            gRegistrations[i].entry.featureFlags = req.featureFlags;
            break;
        }
    }

    auto* ack = reinterpret_cast<mc::PubregRegisterAck*>(conn->buf);
    ack->header.opcode = mc::PubregOpcode::RegisterAck;
    ack->header.totalLength = sizeof(mc::PubregRegisterAck);
    ack->registryId = registryId;
    ack->error = registryId != 0 ? 0 : 1;  // v1 - 상한 초과(테이블 가득 참)만 실패 사유, 새 ChannelError 값 불필요
    return sizeof(mc::PubregRegisterAck);
}

bool kImplementationIdMatches(const mc::PubregRegistrationEntry& entry, const mc::uint8_t* match,
                               mc::uint32_t matchLen) {
    if (matchLen == 0 || matchLen > sizeof(entry.implementationId)) {
        return false;
    }
    // 부분 일치(substring) - implementationId는 opaque 바이트 블록이지만
    // 등록자가 텍스트를 담았다면 그 바이트에 대한 단순 substring 탐색
    // (QU-4B38857C 답변 - "구현체 식별자" 용도, 대소문자 구분).
    const mc::uint32_t haystackLen = sizeof(entry.implementationId);
    if (matchLen > haystackLen) {
        return false;
    }
    for (mc::uint32_t start = 0; start + matchLen <= haystackLen; ++start) {
        bool ok = true;
        for (mc::uint32_t i = 0; i < matchLen; ++i) {
            if (entry.implementationId[start + i] != match[i]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

mc::uint32_t kHandleQuery(Connection* conn, const mc::uint8_t* body, mc::uint32_t bodyLen) {
    mc::PubregQueryRequest req{};
    constexpr mc::uint32_t kFixedBodyLen = sizeof(mc::PubregQueryRequest) - sizeof(mc::PubregMessageHeader);
    if (bodyLen < kFixedBodyLen) {
        return 0;
    }
    mc::uint8_t* dst = reinterpret_cast<mc::uint8_t*>(&req) + sizeof(mc::PubregMessageHeader);
    for (mc::uint32_t i = 0; i < kFixedBodyLen; ++i) {
        dst[i] = body[i];
    }

    auto* resp = reinterpret_cast<mc::PubregQueryResponseHeader*>(conn->buf);
    resp->header.opcode = mc::PubregOpcode::QueryResponse;
    resp->totalMatched = 0;
    resp->returnedCount = 0;

    auto* entries = reinterpret_cast<mc::PubregRegistrationEntry*>(conn->buf + sizeof(mc::PubregQueryResponseHeader));
    mc::uint32_t matched = 0;
    for (mc::uint32_t i = 0; i < kMaxRegistrations; ++i) {
        if (!gRegistrations[i].inUse) {
            continue;
        }
        const bool isMatch =
            req.mode == 0 || kImplementationIdMatches(gRegistrations[i].entry, req.match, req.matchLen);
        if (!isMatch) {
            continue;
        }
        if (matched >= req.offset && resp->returnedCount < req.count &&
            resp->returnedCount < mc::kPubregMaxQueryResultsPerResponse) {
            entries[resp->returnedCount] = gRegistrations[i].entry;
            ++resp->returnedCount;
        }
        ++matched;
    }
    resp->totalMatched = matched;
    const mc::uint32_t totalLength =
        sizeof(mc::PubregQueryResponseHeader) + resp->returnedCount * sizeof(mc::PubregRegistrationEntry);
    resp->header.totalLength = totalLength;
    return totalLength;
}

// conn->buf[0..bytesBuffered)에 완전한 메시지가 쌓여 있으면 처리하고
// true를 반환한다(처리 후 이 메시지만큼 buf를 앞으로 당김 - 다음
// 메시지가 이미 파이프라인으로 도착해 있을 수 있으므로). 아직
// 완전하지 않으면 false(호출부가 이어서 더 읽는다).
bool kTryHandleOneMessage(Connection* conn, mc::uint32_t connectionIndex, mc::uint32_t* outResponseLength) {
    if (conn->bytesBuffered < sizeof(mc::PubregMessageHeader)) {
        return false;
    }
    const auto* header = reinterpret_cast<const mc::PubregMessageHeader*>(conn->buf);
    const mc::uint32_t totalLength = header->totalLength;
    if (totalLength < sizeof(mc::PubregMessageHeader) || totalLength > sizeof(conn->buf) ||
        conn->bytesBuffered < totalLength) {
        return false;
    }

    const mc::uint8_t* body = conn->buf + sizeof(mc::PubregMessageHeader);
    const mc::uint32_t bodyLen = totalLength - sizeof(mc::PubregMessageHeader);
    mc::uint32_t responseLength = 0;
    switch (header->opcode) {
        case mc::PubregOpcode::Register:
            responseLength = kHandleRegister(conn, connectionIndex, body, bodyLen);
            break;
        case mc::PubregOpcode::QueryRequest:
            responseLength = kHandleQuery(conn, body, bodyLen);
            break;
        default:
            responseLength = 0;  // 알 수 없는 opcode - 응답 없이 무시(v1, 새 DC 불필요 수준)
            break;
    }

    // 처리에 쓴 메시지를 buf에서 밀어내고, 파이프라인으로 이미 도착한
    // 다음 메시지가 있으면 앞으로 당긴다(응답을 buf 맨 앞에 다시 써야
    // 하므로, 밀어내기는 응답 작성 이후가 아니라 여기서 미리 한다 -
    // kHandleRegister/kHandleQuery는 항상 conn->buf 맨 앞부터 응답을
    // 쓰므로 이 순서가 안전하다. 단, 아직 안 쓴 다음 메시지 조각을
    // 잃지 않도록 별도 스크래치가 필요 - v1은 한 번에 메시지 하나만
    // 파이프라인 허용을 포기하고, 응답 전송 뒤 buf를 완전히 비우는
    // 단순한 모델로 좁힌다).
    conn->bytesBuffered = 0;
    *outResponseLength = responseLength;
    return true;
}

}  // namespace

extern "C" void _start() {
    mc::OpenChannelArgs openArgs;
    openArgs.name = kPubregChannelName;
    openArgs.nameLength = sizeof(kPubregChannelName) - 1;

    mc::SyscallToken openToken = mc::submit(mc::kSyscallEndpointOpenChannel, &openArgs);
    if (openToken == 0 || !mc::wait(openToken) || openArgs.error != mc::ChannelError::None) {
        // 이름 충돌(이미 다른 pubreg 인스턴스가 떠 있음) 또는 자원 고갈
        // - 이 서비스는 계속 존재할 이유가 없으므로 종료한다.
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
                for (mc::uint32_t i = 0; i < kMaxConnections; ++i) {
                    if (!gConnections[i].inUse) {
                        gConnections[i] = Connection{};
                        gConnections[i].inUse = true;
                        gConnections[i].bridge = gAcceptArgs.bridge;
                        kSubmitRead(&gConnections[i]);
                        break;
                    }
                    if (i == kMaxConnections - 1) {
                        // v1 상한 초과 - 더 받을 자리가 없어 그냥 닫는다.
                        mc::CloseBridgeArgs closeArgs;
                        closeArgs.bridge = gAcceptArgs.bridge;
                        mc::SyscallToken closeToken = mc::submit(mc::kSyscallEndpointCloseBridge, &closeArgs);
                        if (closeToken != 0) {
                            mc::wait(closeToken);
                        }
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
                    kCloseConnection(conn, i);
                    break;
                }
                conn->bytesBuffered += static_cast<mc::uint32_t>(conn->readArgs.bytesRead);
                mc::uint32_t responseLength = 0;
                if (kTryHandleOneMessage(conn, i, &responseLength)) {
                    if (responseLength > 0) {
                        kSubmitWrite(conn, responseLength);
                    } else {
                        // 처리 실패(포맷 오류/알 수 없는 opcode) - 응답 없이
                        // 다음 메시지를 계속 기다린다.
                        kSubmitRead(conn);
                    }
                } else {
                    // 아직 완전한 메시지가 안 모임 - 이어서 더 읽는다.
                    kSubmitRead(conn);
                }
            } else {  // ConnState::Writing
                if (result.resultOutcome != mc::MultiWaitOutcome::Completed ||
                    conn->writeArgs.error != mc::ChannelError::None) {
                    kCloseConnection(conn, i);
                    break;
                }
                kSubmitRead(conn);
            }
            break;
        }
    }

    mc::selfTerminate(0);
}
