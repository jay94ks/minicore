#ifndef MINICORE_KERNEL_USER_RECORD_H
#define MINICORE_KERNEL_USER_RECORD_H

#include "async_task.h"
#include "channel.h"
#include "libkenv/permission.h"
#include "libkenv/types.h"

namespace kernel {

class Process;

// [신규, 2026-09-23, SP-30FCC8AE §1-A, PN-B6DB692C] uid는 authmgr이
// 유일한 권위 있는 저장소를 갖는 트리 구조 - 이 구조체는 그 저장소의
// 커널 read-through 캐시 한 슬롯. 필드 목록은 그 계획의 "UserRecord
// 필드/캐시 구조 확정"(설계자 의견, 2026-09-17) 그대로.
//
// **이번 증분의 의도적 범위 축소**: authmgr에 대한 실제 Channel IPC
// 비동기 질의(cache miss 시 co_await 왕복)는 아직 배선하지 않았다 -
// authmgr 자신의 사용자 조회/등록 요청 종류가 아직 없어(PN-24A2B6F5
// "남은 것" 항목2/3이 이 계획의 스키마 확정을 기다리고 있었으므로
// 순서상 먼저 할 수 없었다) 캐시 미스는 지금은 그냥 실패
// (`ChannelError::ServiceUnavailable`, 설계자가 확정한 정책과 동일한
// 에러 코드 - "authmgr이 없으면 캐시된 범위 내에서만 허가하고 나머지는
// 서비스 불가"를 지금은 "캐시된 범위 = root뿐"인 상태로 이미 만족한다)
// 로 처리한다 - 다음 증분이 authmgr 프로토콜에 조회 요청을 추가하는
// 대로 `insertOrUpdate()`를 그 응답 경로에서 호출하도록 배선하면 된다
// (RM-23F4B687 §4 취지 - 실제 authmgr 요청 종류가 아직 하나도 없는
// 지금 그 배선을 미리 만들면 검증 불가능한 코드가 된다).
constexpr uint32_t kUserRecordMaxCacheEntries = 1024;
constexpr uint32_t kUserRecordMaxLoginNameBytes = 32;
constexpr uint32_t kUserRecordMaxPasswordHashBytes = 96;  // "algorithm:value" 형식, sha256 hex 64자 + 여유
constexpr uint32_t kUserRecordMaxShellBytes = 64;

struct UserRecord {
    Uid uid = kRootUid;
    Uid parentUid = kRootUid;
    Gid gid = kRootGid;
    char loginName[kUserRecordMaxLoginNameBytes] = {};
    char passwordHash[kUserRecordMaxPasswordHashBytes] = {};
    char defaultShell[kUserRecordMaxShellBytes] = {};
    bool valid = false;
    uint64_t lastHitTime = 0;
};

// authmgr의 read-through 캐시(`gUserRecordCache[1024]`) - root(uid=0)
// 는 부팅 시 커널이 직접 하드코딩하고 축출 대상에서 항상 제외된다
// (설계 확정 그대로, authmgr 가용성과 무관하게 root 판정이 항상
// 성립해야 하므로).
class UserRecordCache {
public:
    // BSP에서 1회 호출 - root 엔트리를 하드코딩해 캐시에 심는다.
    static void init();

    // uid로 조회 - 히트 시 lastHitTime을 현재 tick으로 갱신한 뒤
    // 포인터 반환, 미스면 nullptr(이번 증분은 authmgr 비동기 질의로
    // 자동으로 채우지 않는다 - 위 클래스 문서 주석 참고).
    static const UserRecord* lookup(Uid uid);

    // authmgr 질의 응답(또는 부팅 시 root)으로 캐시를 채우거나 갱신.
    // 이미 1024개가 다 찼으면 LRU(가장 오래된 lastHitTime, root 제외)
    // 축출 후 삽입.
    static void insertOrUpdate(const UserRecord& record);
};

// [SP-30FCC8AE §1-A, PN-B6DB692C, kCanSetuid 폐기(설계자 지시) -
// 판정과 실행을 한 번에 하는 시도-실패 패턴] root는 임의 uid로,
// 그 외 사용자는 targetUid의 parentUid 체인을 타고 올라가 callerUid에
// 닿는 경우(자신의 하위)에만 전환 가능 - 별도 "할 수 있는지" 질의
// API는 없다. 성공 시 caller.uid를 즉시 전환하고 gid는 건드리지
// 않는다(POSIX setuid()와 동일 관례 - gid 전환은 별도 setgid류 후속
// 대상). 캐시 미스(targetUid 자체가 캐시에 없거나, 조상 체인 중간의
// 어떤 uid가 캐시에 없어 판정을 끝까지 못 하는 경우)는
// `ChannelError::ServiceUnavailable`.
ChannelError kSetuid(Process& caller, Uid targetUid);

// [신규, 2026-09-28, DC-CC83F7BE 답변("(A) 커널 중개... kSetuid와
// 대칭적인 조상-자손 규칙") 반영] `kSetuid()`가 내부적으로 쓰던
// 조상-자손 tree-walk을 `CreateUser`의 권한 판정에도 재사용할 수
// 있도록 헤더에 노출한다 - "targetUid에서 parentUid 체인을 타고
// 올라가 callerUid에 닿는지"(=callerUid가 targetUid의 조상이거나
// 자기 자신인지)를 그대로 검사하므로, `CreateUser`가 새로 만들려는
// uid의 선언된 parentUid를 `targetUid` 자리에 넘기면 "callerUid가
// 그 parentUid의 조상이거나 자기 자신일 때만 허용"이라는 원하는
// 의미와 정확히 일치한다. **`kCanSetuid` 폐기 원칙과 무관** - 이건
// "질의 전용 API"가 아니라 `kSetuid()`/`kCreateUserOnExecImpl()`
// 둘 다의 판정+실행이 같은 함수 호출 안에서 곧바로 이어지는 내부
// 구현 세부의 공유일 뿐이다(TOCTOU 창이 없음).
bool kIsDescendantUser(Uid callerUid, Uid targetUid, ChannelError* outLookupFailure);

// [신규, 2026-09-28, DC-90A66932 (A) 채택] `SetuidHandler::onExec()`의
// 실제 구현 - 위 `kSetuid()`로 먼저 시도해 캐시 히트면 즉시 끝내고,
// `ServiceUnavailable`(캐시 미스)이면 `authmgr_client.h`를 통해
// authmgr에 비동기 질의해 캐시를 채운 뒤 다시 `kSetuid()`를 시도한다.
// `process.cpp`의 `SetuidHandler::onExec()`가 이 함수를 그대로
// 반환(위임)하는 방식으로 연결한다 - `AsyncExecCoro`는 다른
// `AsyncExecCoro`를 `co_await`로 합성할 수 없다는 이 프로젝트의 기존
// 제약(ext4/fat32 VFS 통합이 이미 겪음) 때문에, "코루틴을 co_await"가
// 아니라 "코루틴 객체를 그대로 반환"하는 위임으로 연결한다.
AsyncExecCoro kSetuidOnExecImpl(AsyncTask* task, void* argsRaw);

// [신규, 2026-09-28, DC-CC83F7BE 답변 반영] `CreateUserHandler::
// onExec()`의 실제 구현(process.cpp가 kSetuidOnExecImpl과 동일한
// 반환값 위임 패턴으로 연결) - caller의 실제 Process::uid를 얻어
// (root 또는 새 uid의 parentUid에 대한 조상-자손 판정, 위
// kIsDescendantUser 참고) 통과하면 authmgr_client.h로 authmgr에
// CreateUser를 대신 요청한다. kSetuidOnExecImpl과 마찬가지로 이
// 함수는 co_await를 쓰지 않는다(같은 이유 - authmgr_client.h가 쓰는
// AsyncTaskAwaiter와의 dispatch-mode 혼용 금지, DC-90A66932 참고).
AsyncExecCoro kCreateUserOnExecImpl(AsyncTask* task, void* argsRaw);

// [신규, 2026-09-29, DC-2B22FBF0 답변("(A-2) 조상-자손 규칙 재사용")
// 반영] `GrantSudoPermissionHandler::onExec()`의 실제 구현 -
// kCreateUserOnExecImpl과 완전히 같은 패턴(caller의 실제 Process::uid로
// root-또는-조상 판정, 통과하면 authmgr_client.h로 대신 요청). 같은
// 이유로 co_await를 쓰지 않는다.
AsyncExecCoro kGrantSudoPermissionOnExecImpl(AsyncTask* task, void* argsRaw);

}  // namespace kernel

#endif  // MINICORE_KERNEL_USER_RECORD_H
