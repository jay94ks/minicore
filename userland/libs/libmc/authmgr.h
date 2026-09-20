#ifndef USERLAND_LIBS_LIBMC_MC_AUTHMGR_H
#define USERLAND_LIBS_LIBMC_MC_AUTHMGR_H

#include "libmc/types.h"

// authmgr(SP-30FCC8AE §1-D, PN-24A2B6F5/PN-BDEAA9B5)의 Channel IPC
// 프로토콜 v1 - "바이너리로 직렬화해서 Request/Response/Notification
// 구조로 구성, Request 자체에 요청 구분을 넣어 다수 채널 불필요"라는
// 설계자 확정을 그대로 따른다. **이 파일은 authmgr 프로토콜의 첫
// 구현이다** - PN-24A2B6F5가 "authmgr 프로토콜을 먼저 구현 → 그
// 구현을 일반화해 libkproto로 추출"이라는 순서를 고정해 뒀으므로,
// 지금은 pubreg.h처럼 authmgr 전용으로 두고 나중에 실제 요청 종류가
// 늘어난 뒤 공통 프레이밍만 libkproto로 뽑아낸다(역순 불가).
//
// v1은 UserRecord/libkvdb 연동이 아직 없어(선행 조건 미충족) 실제
// 업무 요청 없이 프레이밍 자체(Request/Response/Notification 3종 +
// Request 내부 discriminator)와 왕복(Ping/Pong) 하나만 확립한다.
// pubreg.h와 마찬가지로 `requestId` 같은 다중 요청 상관관계 필드는
// 뺐다 - 지금은 연결마다 한 번에 요청 하나만 순차 처리하므로
// 상관관계를 구분할 필요가 없다(과설계 방지, RM-23F4B687 §4).

namespace mc {

// Channel IPC(SP-1FBC0EEB)는 raw binary 스트림이라 메시지 경계가
// 없으므로, 모든 메시지는 자기 전체 길이를 담은 고정 헤더로 시작한다
// (리틀 엔디안, x86_64 네이티브 - 이 채널은 항상 로컬 프로세스 간,
// pubreg.h의 PubregMessageHeader와 동일한 관례).
enum class AuthmgrFrameKind : uint8_t {
    Request = 1,
    Response = 2,
    Notification = 3,  // v1은 생산자가 없음 - 프레임 종류만 예약.
};

struct AuthmgrMessageHeader {
    uint32_t totalLength = 0;  // 이 헤더 포함 메시지 전체 바이트 수
    AuthmgrFrameKind frameKind = AuthmgrFrameKind::Request;
    uint8_t reserved[3] = {};  // 정렬용, 항상 0
};

// Request 프레임 자신이 담는 요청 종류 discriminator - "Request
// 자체에 요청 구분을 넣으면 다수의 채널을 열 필요가 없다"는 설계자
// 확정 그대로. v1은 왕복 자체를 증명하는 Ping 하나뿐 - UserRecord
// 조회/등록/인증 등은 libkvdb가 생긴 뒤 여기 추가된다.
enum class AuthmgrRequestType : uint8_t {
    Ping = 1,
};

struct AuthmgrRequestHeader {
    AuthmgrMessageHeader header;  // frameKind = Request
    AuthmgrRequestType requestType = AuthmgrRequestType::Ping;
    uint8_t reserved[3] = {};
    // requestType별 고정 폭 본문이 있다면 이 구조체 바로 뒤에 이어짐
    // (v1은 Ping뿐이고 본문 없음).
};

struct AuthmgrResponseHeader {
    AuthmgrMessageHeader header;  // frameKind = Response
    AuthmgrRequestType requestType = AuthmgrRequestType::Ping;  // 어느 요청에 대한 응답인지
    uint8_t reserved[3] = {};
    uint32_t error = 0;  // mc::ChannelError 값 재사용(0=None) - requestType을
                          // 모르는 요청이면 NotSupported류로 채워 반환.
    // requestType별 고정 폭 응답 본문이 있다면 이 구조체 바로 뒤에
    // 이어짐(v1의 Pong은 본문 없음 - error==None이면 그 자체가 응답).
};

// 한 메시지(요청이든 응답이든)의 최대 바이트 수 - Channel IPC 스트림
// 위에서 메시지 경계를 프레이밍하는 쪽이 이 크기의 버퍼를 준비해
// 둔다(pubreg.h의 kPubregMaxMessageBytes와 동일한 관례). v1은 고정
// 크기 헤더뿐이라 여유를 조금만 둔다 - 실제 요청 본문이 추가되면
// 그때 다시 계산.
constexpr uint32_t kAuthmgrMaxMessageBytes = 128;

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_AUTHMGR_H
