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

// [신규, 2026-09-28, DC-90A66932 (A) 채택] 위 kSetuid()가 캐시 미스로
// ServiceUnavailable을 돌려주면, authmgr_client.h를 통해 authmgr에
// LookupByUid로 비동기 질의해 캐시를 채운 뒤 kSetuid()를 다시 시도한다
// - 동시 호출은 고정 연결 하나를 공유하므로 kAuthmgrClientLock()/
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

    ChannelError result = kSetuid(*proc, args->targetUid);
    if (result != ChannelError::ServiceUnavailable) {
        args->error = result;
        co_return;
    }

    kAuthmgrClientLock();
    UserRecord fetched{};
    bool queried = false;
    AsyncTask* connectTask = kAuthmgrBeginConnect();
    if (connectTask) {
        // [실측으로 확정, 2026-09-28] 이 함수(kSetuidOnExecImpl)는 이
        // 지점을 포함해 끝까지 `co_await`를 절대 쓰지 않는다 - 한 번이라도
        // 진짜 C++ `co_await`로 정지하면 이 AsyncTask는 drainOnce()의
        // coroutine-handle 재개 모드로 영구 전환되는데(async_task.cpp
        // drainOnce() 문서 주석), 바로 아래
        // `kAuthmgrWriteLookupRequest`/`kAuthmgrReadLookupResponse`가
        // 내부적으로 쓰는 raw `AsyncTask::yield()`(스택풀 전용
        // kContextSwitch)와 섞이면 이미 못 쓰게 된 재개 지점으로 잘못
        // 점프해 실행이 중복/오염된다(setuidtest E2E 검증 중 응답 처리가
        // 두 번 실행되는 것으로 실측 확인). 그렇다고 `co_await` 없이 단순
        // `while (!done) AsyncTask::yield();`도 안 된다 - 아무도 이
        // AsyncTask를 다시 큐에 넣어 주지 않아 첫 yield에서 영원히
        // 멈춘다. `AsyncTaskAwaiter::await()`가 바로 이 조합(스택풀
        // 모드 + "임의의 관계없는 AsyncTask 기다리기")을 위해 이미
        // "매 yield 직전 스스로를 submitCompletion()으로 재제출"을
        // 구현해 뒀으므로 그대로 재사용한다 - "코루틴 안에서 부르면
        // 무한 대기"라는 그 클래스의 경고는 호출자 자신이 이미
        // coroHandle 모드로 전환된 뒤에만 해당하고(Ext4Driver 사례),
        // 이 함수는 그 상태에 절대 들어가지 않으므로 안전하다.
        AsyncTaskAwaiter(connectTask).await();
        kAuthmgrFinishConnect(connectTask);
    }
    if (kAuthmgrWriteLookupRequest(args->targetUid) && kAuthmgrReadLookupResponse(&fetched)) {
        queried = true;
    }
    kAuthmgrClientUnlock();

    if (!queried) {
        args->error = ChannelError::ServiceUnavailable;
        co_return;
    }
    UserRecordCache::insertOrUpdate(fetched);
    args->error = kSetuid(*proc, args->targetUid);
    co_return;
}

}  // namespace kernel
