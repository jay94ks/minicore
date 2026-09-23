#include "user_record.h"

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

}  // namespace kernel
