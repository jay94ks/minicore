#include "user_record.h"

#include "authmgr_client.h"
#include "process.h"
#include "timer.h"

namespace kernel {

namespace {

UserRecord gUserRecordCache[kUserRecordMaxCacheEntries];
uint32_t gUserRecordCacheCount = 0;  // 실제 사용 중인 슬롯 수(뒤에서부터가 아니라 앞에서부터 채움)

const UserRecord* kFindUserRecordSlot(Uid uid) {
    for (uint32_t i = 0; i < gUserRecordCacheCount; ++i) {
        if (gUserRecordCache[i].valid && gUserRecordCache[i].uid == uid) {
            return &gUserRecordCache[i];
        }
    }
    return nullptr;
}

UserRecord* kFindUserRecordSlotMutable(Uid uid) {
    return const_cast<UserRecord*>(kFindUserRecordSlot(uid));
}

// LRU 축출 대상 - root(uid==kRootUid)는 절대 후보에 안 넣는다(설계
// 확정). 가장 오래된 lastHitTime을 가진 슬롯의 인덱스, 없으면
// kUserRecordMaxCacheEntries.
uint32_t kFindLruEvictionCandidate() {
    uint32_t oldestIndex = kUserRecordMaxCacheEntries;
    uint64_t oldestHit = 0;
    for (uint32_t i = 0; i < gUserRecordCacheCount; ++i) {
        if (!gUserRecordCache[i].valid || gUserRecordCache[i].uid == kRootUid) {
            continue;
        }
        if (oldestIndex == kUserRecordMaxCacheEntries || gUserRecordCache[i].lastHitTime < oldestHit) {
            oldestIndex = i;
            oldestHit = gUserRecordCache[i].lastHitTime;
        }
    }
    return oldestIndex;
}

}  // namespace

void UserRecordCache::init() {
    // `gUserRecordCache`는 정적 저장 기간 배열이라 UserRecord의 기본
    // 멤버 초기화값(전부 상수식)으로 이미 0/false로 존재한다 - 별도
    // memset이 필요 없다.
    gUserRecordCacheCount = 0;

    // [설계 확정, 2026-09-17] root(uid=0)는 authmgr 가용성과 무관하게
    // 항상 판정이 성립해야 하므로 부팅 시 커널이 직접 하드코딩한다 -
    // authmgr 기동 후 그쪽이 보관한 진짜 root 레코드로 전환하는 절차는
    // PN-24A2B6F5 후속 증분(이 계획의 범위 밖, 위 user_record.h 문서
    // 주석 참고).
    UserRecord root;
    root.uid = kRootUid;
    root.parentUid = kRootUid;
    root.gid = kRootGid;
    root.valid = true;
    root.lastHitTime = Timer::tickCount();
    gUserRecordCache[0] = root;
    gUserRecordCacheCount = 1;
}

const UserRecord* UserRecordCache::lookup(Uid uid) {
    UserRecord* slot = kFindUserRecordSlotMutable(uid);
    if (!slot) {
        return nullptr;
    }
    slot->lastHitTime = Timer::tickCount();
    return slot;
}

void UserRecordCache::insertOrUpdate(const UserRecord& record) {
    UserRecord* existing = kFindUserRecordSlotMutable(record.uid);
    if (existing) {
        *existing = record;
        existing->valid = true;
        existing->lastHitTime = Timer::tickCount();
        return;
    }

    if (gUserRecordCacheCount < kUserRecordMaxCacheEntries) {
        UserRecord& slot = gUserRecordCache[gUserRecordCacheCount++];
        slot = record;
        slot.valid = true;
        slot.lastHitTime = Timer::tickCount();
        return;
    }

    const uint32_t evictIndex = kFindLruEvictionCandidate();
    if (evictIndex == kUserRecordMaxCacheEntries) {
        // 캐시가 root 하나로만 가득 찬 이론상 불가능한 상태(root는
        // 슬롯 1개뿐이라 kUserRecordMaxCacheEntries==1이 아닌 한
        // 실제로 도달하지 않음) - 조용히 삽입을 포기한다(정직한 실패,
        // RM-23F4B687 §4 - 이 경계 케이스에 새 에러 코드를 만들 만큼
        // 실사용 가능성이 없다고 판단).
        return;
    }
    UserRecord& slot = gUserRecordCache[evictIndex];
    slot = record;
    slot.valid = true;
    slot.lastHitTime = Timer::tickCount();
}

// [SP-30FCC8AE §1-A, PN-B6DB692C] targetUid의 parentUid 체인을 최대
// kUserRecordMaxCacheEntries번(캐시에 있을 수 있는 최대 조상 수)까지
// 거슬러 올라가며 callerUid에 닿는지 확인 - 그보다 더 돌면 사이클이
// 있다는 뜻이므로(정상 데이터라면 있을 수 없음) 안전하게 실패 처리.
bool kIsDescendantUser(Uid callerUid, Uid targetUid, ChannelError* outLookupFailure) {
    Uid cursor = targetUid;
    for (uint32_t i = 0; i < kUserRecordMaxCacheEntries; ++i) {
        if (cursor == callerUid) {
            return true;
        }
        if (cursor == kRootUid) {
            return false;  // root까지 올라갔는데도 callerUid를 못 만남
        }
        const UserRecord* record = UserRecordCache::lookup(cursor);
        if (!record) {
            *outLookupFailure = ChannelError::ServiceUnavailable;
            return false;
        }
        cursor = record->parentUid;
    }
    // 사이클(또는 비정상적으로 긴 체인) - 데이터 오류로 간주해 거부.
    *outLookupFailure = ChannelError::ServiceUnavailable;
    return false;
}

ChannelError kSetuid(Process& caller, Uid targetUid) {
    if (targetUid == caller.uid) {
        return ChannelError::None;  // POSIX setuid(현재 uid)와 동일 - no-op 성공
    }

    const UserRecord* target = UserRecordCache::lookup(targetUid);
    if (!target) {
        return ChannelError::ServiceUnavailable;
    }

    if (caller.uid == kRootUid) {
        caller.uid = targetUid;
        return ChannelError::None;
    }

    ChannelError lookupFailure = ChannelError::None;
    if (kIsDescendantUser(caller.uid, targetUid, &lookupFailure)) {
        caller.uid = targetUid;
        return ChannelError::None;
    }
    if (lookupFailure != ChannelError::None) {
        return lookupFailure;
    }
    return ChannelError::PermissionDenied;
}

// [신규, 2026-09-28, DC-90A66932 (A) 채택 + DC-34764C25 항목1 답변]
// 위 kSetuid()가 캐시 미스로 ServiceUnavailable을 돌려주면,
// authmgr_client.h를 통해 authmgr에 LookupByUid로 비동기 질의해
// 캐시를 채운 뒤 kSetuid()를 다시 시도한다. 그래도 PermissionDenied
// (root도 조상-자손도 아님)면 authmgr의 sudo 화이트리스트를 마지막
// 수단으로 질의한다(sudo/su - "sudo -u <유저>"류, root가 아닌 대상도
// 가능). 동시 호출은 고정 연결 하나를 공유하므로 kAuthmgrClientLock()/
// Unlock()으로 반드시 직렬화한다(연결 자체는 authmgr_client.cpp의
// 전역 상태로 고정 유지 - 끊어지면 다음 호출이 자동 재연결 시도).
AsyncExecCoro kSetuidOnExecImpl(AsyncTask* task, void* argsRaw) {
    auto* args = static_cast<SetuidArgs*>(argsRaw);

    SharedPtr<Task> submitter = task->submitterTask.lock();
    auto* caller = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
    SharedPtr<Process> proc = caller ? caller->process.lock() : SharedPtr<Process>();
    if (!proc) {
        args->error = ChannelError::NotFound;
        co_return;
    }

    // [실측으로 확정, 2026-09-28] 이 함수(kSetuidOnExecImpl)는 끝까지
    // `co_await`를 절대 쓰지 않는다 - 한 번이라도 진짜 C++ `co_await`로
    // 정지하면 이 AsyncTask는 drainOnce()의 coroutine-handle 재개
    // 모드로 영구 전환되는데(async_task.cpp drainOnce() 문서 주석),
    // 아래 `kAuthmgr*` 함수들이 내부적으로 쓰는 raw `AsyncTask::yield()`
    // (스택풀 전용 kContextSwitch)와 섞이면 이미 못 쓰게 된 재개
    // 지점으로 잘못 점프해 실행이 중복/오염된다(setuidtest E2E 검증
    // 중 실측 확인). `kAuthmgrEnsureConnected()`가 내부적으로 쓰는
    // `AsyncTaskAwaiter::await()`(매 yield 직전 스스로를
    // submitCompletion()으로 재제출)가 이 스택풀 전용 조합을 위한
    // 올바른 대기자다 - "코루틴 안에서 부르면 무한 대기"라는 그
    // 클래스의 경고는 호출자가 이미 coroHandle 모드로 전환된 뒤에만
    // 해당한다(Ext4Driver 사례).
    ChannelError result = kSetuid(*proc, args->targetUid);
    if (result == ChannelError::ServiceUnavailable) {
        kAuthmgrClientLock();
        UserRecord fetched{};
        const bool queried =
            kAuthmgrEnsureConnected() && kAuthmgrWriteLookupRequest(args->targetUid) && kAuthmgrReadLookupResponse(&fetched);
        kAuthmgrClientUnlock();
        if (!queried) {
            args->error = ChannelError::ServiceUnavailable;
            co_return;
        }
        UserRecordCache::insertOrUpdate(fetched);
        result = kSetuid(*proc, args->targetUid);
    }

    // [신규, 2026-09-28, DC-34764C25 항목1 답변] 캐시는 채워졌지만(두
    // uid 모두 유효) root도 아니고 조상-자손 관계도 아니라
    // `PermissionDenied`인 경우 - authmgr의 sudo 화이트리스트를 마지막
    // 수단으로 질의한다("authmgr 내부의 별도 화이트 리스트, 계정별로
    // 화이트 리스트가 별도로 존재" - sudo -u <root가 아닌 유저>도
    // 허용).
    if (result == ChannelError::PermissionDenied) {
        kAuthmgrClientLock();
        const bool allowed = kAuthmgrEnsureConnected() && kAuthmgrCheckSudoPermission(proc->uid, args->targetUid);
        kAuthmgrClientUnlock();
        if (allowed) {
            proc->uid = args->targetUid;
            result = ChannelError::None;
        }
    }

    args->error = result;
    co_return;
}

// [신규, 2026-09-28, DC-CC83F7BE 답변("(A) 커널 중개, 최종 권한
// 판정은 커널이") 반영] CreateUserHandler(process.cpp)가 위임하는
// 실제 구현. 캐시미스 재시도가 있는 kSetuidOnExecImpl과 달리, 여기서
// kIsDescendantUser()가 조상 체인 중간에서 캐시 미스를 만나면 그냥
// ServiceUnavailable로 정직하게 실패한다(재시도 안 함) - 어떤 uid가
// 미스였는지 이 함수 시그니처로는 알 수 없어(kSetuid의 "정확히
// targetUid 하나"와 달리 체인 전체가 대상) authmgr 재질의 대상을
// 특정할 수 없기 때문(RM-23F4B687 §4 - 실사용 없이 추측으로 범위를
// 넓히지 않음, 필요해지면 후속 세션이 kIsDescendantUser의 실패
// 지점을 함께 반환하도록 확장).
AsyncExecCoro kCreateUserOnExecImpl(AsyncTask* task, void* argsRaw) {
    auto* args = static_cast<CreateUserArgs*>(argsRaw);

    SharedPtr<Task> submitter = task->submitterTask.lock();
    auto* caller = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
    SharedPtr<Process> proc = caller ? caller->process.lock() : SharedPtr<Process>();
    if (!proc) {
        args->error = ChannelError::NotFound;
        co_return;
    }

    if (proc->uid != kRootUid) {
        ChannelError lookupFailure = ChannelError::None;
        if (!kIsDescendantUser(proc->uid, args->parentUid, &lookupFailure)) {
            args->error = lookupFailure != ChannelError::None ? lookupFailure : ChannelError::PermissionDenied;
            co_return;
        }
    }

    UserRecord record{};
    record.uid = args->uid;
    record.parentUid = args->parentUid;
    record.gid = args->gid;
    memcpy(record.loginName, args->loginName, sizeof(record.loginName));
    memcpy(record.passwordHash, args->passwordHash, sizeof(record.passwordHash));
    memcpy(record.defaultShell, args->defaultShell, sizeof(record.defaultShell));

    kAuthmgrClientLock();
    const bool created = kAuthmgrEnsureConnected() && kAuthmgrCreateUser(record);
    kAuthmgrClientUnlock();
    if (created) {
        // [신규, 2026-09-28] 방금 만든 레코드를 커널 자신의 read-through
        // 캐시에도 즉시 채운다 - 안 그러면 같은 세션에서 곧바로 그
        // uid를 parentUid로 삼아 다시 CreateUser(손자 생성)나 Setuid를
        // 시도할 때 kIsDescendantUser()/kSetuid()가 이 uid를 캐시
        // 미스로만 보게 돼(authmgr LookupByUid를 통해서만 채워지는
        // 기존 경로를 아직 안 거쳤으므로) ServiceUnavailable로 정직하게
        // 실패한다 - 실측(createusertest, uid 61의 자식 63 생성
        // 시도)으로 발견한 실제 버그. 이 레코드는 지금 막 kernel이 직접
        // authmgr에 만들라고 보낸 값 그대로이니 authmgr에 다시 왕복
        // 조회할 필요 없이 그대로 신뢰해 캐시에 넣는다.
        UserRecordCache::insertOrUpdate(record);
    }
    args->error = created ? ChannelError::None : ChannelError::ServiceUnavailable;
    co_return;
}

}  // namespace kernel
