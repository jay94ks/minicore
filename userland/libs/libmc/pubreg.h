#ifndef USERLAND_LIBS_LIBMC_MC_PUBREG_H
#define USERLAND_LIBS_LIBMC_MC_PUBREG_H

#include "libmc/types.h"

// pubreg(SP-B071E628 §3/§6-6, PN-185406F6 항목4)의 register/query
// 프로토콜 와이어 포맷 - 커널 syscall이 아니라 순수 유저-유저 Channel
// IPC 메시지지만, 등록자/소비자/pubreg 자신이 정확히 같은 바이트
// 레이아웃을 공유해야 해서 channel.h/vfs.h와 같은 자리(libmc)에 둔다.
// **이 프로토콜에는 JSON이 전혀 없다** - §6-6(QU-4B38857C 답변)로
// register/query 자체가 완전 바이너리 고정 구조체로 확정됐고, MCP
// 스타일 tool 선언(name/description/JSON Schema)은 이 프로토콜 위에
// 얹히는 "응용 프로그램" 계층의 몫으로 재배치됐다(§6-6 5번 답변).

namespace mc {

// Channel IPC(SP-1FBC0EEB)는 raw binary 스트림이라 메시지 경계가
// 없으므로, 모든 메시지는 자기 전체 길이를 담은 고정 헤더로 시작한다
// (리틀 엔디안, x86_64 네이티브 - 이 채널은 항상 로컬 프로세스 간).
enum class PubregOpcode : uint8_t {
    Register = 1,
    RegisterAck = 2,
    QueryRequest = 3,
    QueryResponse = 4,
};

struct PubregMessageHeader {
    uint32_t totalLength = 0;  // 이 헤더 포함 메시지 전체 바이트 수
    PubregOpcode opcode = PubregOpcode::Register;
    uint8_t reserved[3] = {};  // 정렬용, 항상 0
};

// [SP-B071E628 §6-6] "엔드포인트용 IPC 식별자"는 채널ID 하나로 고정할
// 수 없다("IPC 채널일 수도 있고 TCP/UDP 등등일 수도 있다") - kind 태그
// + 고정폭 payload로 discriminate한다. 지금은 Channel만 실제로
// 쓰인다(Tcp/Udp는 그 전송을 실제로 쓰는 첫 프로토콜이 나올 때 정확한
// payload 레이아웃을 확정 - 과설계 방지).
enum class PubregEndpointKind : uint8_t {
    Channel = 0,
    Tcp = 1,
    Udp = 2,
};

struct PubregEndpoint {
    PubregEndpointKind kind = PubregEndpointKind::Channel;
    uint8_t reserved[3] = {};
    uint8_t payload[24] = {};  // Channel: payload[0..7]=uint64 channelId(LE), 나머지 0
};

inline void kPubregEndpointSetChannel(PubregEndpoint* endpoint, uint64_t channelId) {
    endpoint->kind = PubregEndpointKind::Channel;
    for (uint32_t i = 0; i < sizeof(endpoint->reserved); ++i) {
        endpoint->reserved[i] = 0;
    }
    for (uint32_t i = 0; i < sizeof(endpoint->payload); ++i) {
        endpoint->payload[i] = 0;
    }
    for (uint32_t i = 0; i < sizeof(channelId); ++i) {
        endpoint->payload[i] = static_cast<uint8_t>((channelId >> (i * 8)) & 0xFF);
    }
}

inline uint64_t kPubregEndpointGetChannelId(const PubregEndpoint& endpoint) {
    uint64_t channelId = 0;
    for (uint32_t i = 0; i < sizeof(channelId); ++i) {
        channelId |= static_cast<uint64_t>(endpoint.payload[i]) << (i * 8);
    }
    return channelId;
}

// [SP-B071E628 §6-6] register 요청 본문(registryId 없음 - pubreg가
// 등록순으로 할당해 RegisterAck으로 돌려준다). protocolCode는
// RM-085694F8에서 사전 예약한 정적 코드, implementationId는 등록자가
// 자유롭게 정하는 opaque "구현체 식별자"(§6-6 답변 2 - pubreg는
// 해석하지 않고 그대로 보관/반환).
struct PubregRegisterRequest {
    PubregMessageHeader header;
    uint8_t protocolCode[4] = {};
    uint8_t implementationId[28] = {};
    PubregEndpoint endpoint;
    uint32_t featureFlags = 0;
};

struct PubregRegisterAck {
    PubregMessageHeader header;
    uint32_t registryId = 0;  // 실패 시 0(등록순 할당은 1부터 시작 - 0은 "없음"으로 예약)
    uint32_t error = 0;       // mc::ChannelError 값 재사용(0=None)
};

// query - 전체 획득(mode=0) 또는 implementationId에 대한 부분 일치
// (mode=1, QU-E05A55AD 1번 답변) - 페이지네이션(offset/count) 필수.
struct PubregQueryRequest {
    PubregMessageHeader header;
    uint8_t mode = 0;         // 0=전체, 1=implementationId 부분 일치
    uint8_t reserved[3] = {};
    uint8_t match[28] = {};   // mode==1일 때만 유효
    uint32_t matchLen = 0;    // match의 유효 길이(<=28)
    uint32_t offset = 0;
    uint32_t count = 0;
};

// query 응답에 실리는 등록 항목 하나 - PubregRegisterRequest와 같은
// 필드에 registryId만 더해진 모양(등록 시 받은 값 그대로 반환).
struct PubregRegistrationEntry {
    uint32_t registryId = 0;
    uint8_t protocolCode[4] = {};
    uint8_t implementationId[28] = {};
    PubregEndpoint endpoint;
    uint32_t featureFlags = 0;
};

struct PubregQueryResponseHeader {
    PubregMessageHeader header;
    uint32_t totalMatched = 0;   // 페이지네이션 전체(offset/count 무관, 조건에 맞는 전체 수)
    uint32_t returnedCount = 0;  // 이번 응답에 실제로 담긴 개수(<= 요청 count)
    // returnedCount번 반복: PubregRegistrationEntry (이 구조체 바로 뒤에 이어짐)
};

// v1 상한(고정 상한 컨테이너 관례, RM-23F4B687 §4) - 실측 후 조정.
// 한 QueryResponse 메시지에 담을 수 있는 최대 항목 수 - 이보다 많이
// 요청해도 이 상한으로 잘린다(totalMatched로 전체 개수는 알 수 있어
// 호출부가 offset을 늘려 나머지를 다시 요청할 수 있다).
constexpr uint32_t kPubregMaxQueryResultsPerResponse = 8;

// 한 메시지(요청이든 응답이든)의 최대 바이트 수 - Channel IPC 스트림
// 위에서 메시지 경계를 프레이밍하는 쪽(pubreg/등록자/소비자 전부)이
// 이 크기의 버퍼를 준비해 둔다.
constexpr uint32_t kPubregMaxMessageBytes =
    sizeof(PubregQueryResponseHeader) + kPubregMaxQueryResultsPerResponse * sizeof(PubregRegistrationEntry);

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_PUBREG_H
