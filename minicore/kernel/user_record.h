#ifndef MINICORE_KERNEL_USER_RECORD_H
#define MINICORE_KERNEL_USER_RECORD_H

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

}  // namespace kernel

#endif  // MINICORE_KERNEL_USER_RECORD_H
