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
// 확정 그대로.
enum class AuthmgrRequestType : uint8_t {
    Ping = 1,
    // [신규, 2026-09-23, PN-24A2B6F5/PN-B6DB692C] uid로 UserRecord
    // 조회 - authmgr의 libkvdb Store가 유일한 권위 있는 저장소.
    // 커널의 UserRecordCache 미스 시 이 요청으로 채운다(배선 자체는
    // 아직 후속 - 이 증분은 authmgr 쪽만).
    LookupByUid = 2,
    // [신규, 2026-09-23] 새 UserRecord 생성. **[갱신, 2026-09-28,
    // DC-CC83F7BE 답변("(A) 커널 중개")]** 이제 `mc::process::
    // kSyscallEndpointCreateUser`(그룹0 call12)를 통해 정식으로
    // 노출된다 - 커널이 caller의 실제 uid로 조상-자손 판정(Setuid와
    // 대칭)을 마친 뒤 `authmgr_client.h`로 이 요청을 대신 보낸다.
    // **여전히 남은 잔여 공백**: 이 Channel 자체(이름 "authmgr")에
    // 직접 연결해 이 요청을 보내는 경로는 여전히 무검증이다 - 설계자가
    // (B) Channel 레벨 peer 신원 첨부를 선택하지 않아 authmgr 자신은
    // "누가 보냈는지" 구분할 방법이 없다(DC-CC83F7BE 참고, 의도적
    // 범위 밖). 이 필드 셋(uid/parentUid/gid/loginName/passwordHash/
    // defaultShell)을 커널 쪽 `AuthmgrUserRecord`와 1:1로 실어 보내는
    // 관례는 그대로 유지.
    CreateUser = 3,
    // [신규, 2026-09-28, DC-34764C25 항목1 답변 "authmgr 내부의 별도
    // 화이트 리스트 (계정별로 화이트 리스트가 별도로 존재)"] callerUid
    // 가 targetUid로 sudo/su할 자격이 있는지 조회 - error==None이면
    // 허용, PermissionDenied면 불허(본문 없음, CreateUser와 동일한
    // "error만" 응답 관례). 커널의 kSetuid()가 root/조상-자손 판정에
    // 실패했을 때(캐시된 두 uid 모두 유효한데도 PermissionDenied)
    // 마지막 수단으로 이 요청을 보낸다(authmgr_client.h 참고).
    CheckSudoPermission = 4,
    // [신규, 2026-09-28, DC-34764C25 항목1] 화이트리스트에 항목을
    // 추가 - CreateUser와 동일한 이유로 **아직 권한 검사가 전혀
    // 없다**(누가 이 항목을 추가할 수 있는지는 "S 비트를 설정하는
    // 것 자체가 해당 파일의 소유자만 가능해야" 같은 더 넓은 관리
    // API 설계가 필요 - DC-1526389A 답변 대기, 지금은 테스트/시딩
    // 전용).
    GrantSudoPermission = 5,
};

// [신규, 2026-09-23, PN-B6DB692C] `kernel::UserRecord`(user_record.h)
// 의 와이어 표현 - 캐시 전용 필드(valid/lastHitTime)는 뺀 authmgr
// 권위 저장소의 실제 저장 형태 그대로. 필드 크기는 커널 쪽과 정확히
// 일치시켜야 한다(양쪽 다 SP-30FCC8AE §1-A.1 확정값 - 어긋나면 왕복
// 시 잘림/오염).
constexpr uint32_t kAuthmgrLoginNameMaxBytes = 32;
constexpr uint32_t kAuthmgrPasswordHashMaxBytes = 96;
constexpr uint32_t kAuthmgrShellMaxBytes = 64;

struct AuthmgrUserRecord {
    uint32_t uid = 0;
    uint32_t parentUid = 0;
    uint32_t gid = 0;
    char loginName[kAuthmgrLoginNameMaxBytes] = {};
    char passwordHash[kAuthmgrPasswordHashMaxBytes] = {};
    char defaultShell[kAuthmgrShellMaxBytes] = {};
};

struct AuthmgrLookupByUidRequestBody {
    uint32_t uid = 0;
};

// CheckSudoPermission/GrantSudoPermission 공용 본문 - 둘 다 같은
// (callerUid, targetUid) 쌍만 있으면 된다(DC-34764C25 항목1).
struct AuthmgrSudoPermissionRequestBody {
    uint32_t callerUid = 0;
    uint32_t targetUid = 0;
};

struct AuthmgrRequestHeader {
    AuthmgrMessageHeader header;  // frameKind = Request
    AuthmgrRequestType requestType = AuthmgrRequestType::Ping;
    uint8_t reserved[3] = {};
    // requestType별 고정 폭 본문이 있다면 이 구조체 바로 뒤에 이어짐
    // (Ping은 본문 없음, LookupByUid는 AuthmgrLookupByUidRequestBody,
    // CreateUser는 AuthmgrUserRecord, CheckSudoPermission/
    // GrantSudoPermission은 AuthmgrSudoPermissionRequestBody).
};

struct AuthmgrResponseHeader {
    AuthmgrMessageHeader header;  // frameKind = Response
    AuthmgrRequestType requestType = AuthmgrRequestType::Ping;  // 어느 요청에 대한 응답인지
    uint8_t reserved[3] = {};
    uint32_t error = 0;  // mc::ChannelError 값 재사용(0=None) - requestType을
                          // 모르는 요청이면 NotSupported류로 채워 반환.
    // requestType별 고정 폭 응답 본문이 있다면 이 구조체 바로 뒤에
    // 이어짐(Pong은 본문 없음 - error==None이면 그 자체가 응답.
    // LookupByUid는 error==None일 때만 AuthmgrUserRecord가 이어짐 -
    // NotFound면 본문 없음. CreateUser는 error만, 본문 없음).
};

// 한 메시지(요청이든 응답이든)의 최대 바이트 수 - Channel IPC 스트림
// 위에서 메시지 경계를 프레이밍하는 쪽이 이 크기의 버퍼를 준비해
// 둔다(pubreg.h의 kPubregMaxMessageBytes와 동일한 관례). [갱신,
// 2026-09-23] `AuthmgrResponseHeader`(16) + `AuthmgrUserRecord`(204)
// = 220바이트가 가장 큰 프레임 - 여유를 두고 256으로.
constexpr uint32_t kAuthmgrMaxMessageBytes = 256;

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_AUTHMGR_H
