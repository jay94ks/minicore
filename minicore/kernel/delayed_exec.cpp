#include "delayed_exec.h"

#include "libkenv/spinlock.h"
#include "libkmm/slab.h"
#include "timer.h"

namespace kernel {
namespace {

// [신규, 2026-09-17, PN-73E61BD1 항목3, SP-FAF768AB §0-B] OrderedList/
// List가 요구하는 Traits - `deadlineTick` 기준 오름차순 정렬 +
// `deadlineLink` 연결. `List<DelayedTimerEntry, DeadlineTraits>`(순서
// 무관 임시 버퍼, pump() 참고)도 같은 Traits를 그대로 재사용한다 -
// List는 Key/keyOf를 아예 안 쓰고 Link만 보므로 타입이 호환된다.
struct DeadlineTraits {
    using Key = uint64_t;
    static uint64_t keyOf(const DelayedTimerEntry& entry) { return entry.deadlineTick; }
    static constexpr Node DelayedTimerEntry::* Link = &DelayedTimerEntry::deadlineLink;
};

OrderedList<DelayedTimerEntry, DeadlineTraits> gList;
Spinlock gLock;
uint64_t gNextToken = 1;  // 0은 "실패/무효 토큰" 전용(schedule() 주석 참고)

}  // namespace

void DelayedExecutionQueue::init() {
    SpinlockGuard guard(gLock);
    gList.init();
    gNextToken = 1;
}

uint64_t DelayedExecutionQueue::schedule(uint64_t delayTicks, DelayedCallback callback, void* arg) {
    void* mem = GenericSlabAllocator::alloc(sizeof(DelayedTimerEntry));
    if (!mem) {
        return 0;
    }
    auto* entry = reinterpret_cast<DelayedTimerEntry*>(mem);
    entry->deadlineTick = Timer::tickCount() + delayTicks;
    entry->callback = callback;
    entry->arg = arg;
    // GenericSlabAllocator::alloc()은 raw 메모리를 그대로 내준다
    // (memset(0) 없음, placement new도 없음) - deadlineLink의
    // self-reference 불변조건을 여기서 직접 세워 둔다(libkcont
    // List::init()/Task::waitQueueLink와 동일한 함정, PN-633BF2D8/
    // PN-5ADA7ECA에서 이미 확인된 패턴).
    entry->deadlineLink.prev = &entry->deadlineLink;
    entry->deadlineLink.next = &entry->deadlineLink;

    SpinlockGuard guard(gLock);
    entry->token = gNextToken++;
    gList.insert(entry);
    return entry->token;
}

bool DelayedExecutionQueue::cancel(uint64_t token) {
    SpinlockGuard guard(gLock);
    for (DelayedTimerEntry* cur : gList) {
        if (cur->token == token) {
            OrderedList<DelayedTimerEntry, DeadlineTraits>::remove(cur);
            GenericSlabAllocator::free(cur, sizeof(DelayedTimerEntry));
            return true;
        }
    }
    return false;
}

void DelayedExecutionQueue::pump() {
    // 만료 항목을 리스트에서 먼저 전부 떼어낸 뒤(락 보호 구간은 이
    // 분리 작업까지만), 콜백은 락 밖에서 실행한다 - 콜백이 다시
    // schedule()/cancel()을 호출해도(재진입) 같은 락을 다시 잡다가
    // 자기 자신과 데드락에 빠지지 않는다(기존 동작 그대로 유지).
    //
    // [수정, 2026-09-17, PN-73E61BD1 항목3] gList가 이제
    // deadlineTick 오름차순으로 항상 정렬돼 있으므로, 머리부터 보다가
    // 만료 안 된 첫 항목을 만나면 그 즉시 멈춘다(그 뒤는 전부 마감이
    // 더 늦음) - 예전엔 매번 전체를 끝까지 순회했다. 떼어낸 항목은
    // 순서 무관 임시 List(같은 Traits 재사용, O(1) pushBack/front/
    // remove)에 옮겨 담는다 - OrderedList::insert()로 다시 넣으면
    // 이미 오름차순으로 오는 항목들이라 매번 끝까지 스캔하는 O(n^2)
    // 함정에 빠진다(정렬이 필요 없는 임시 버퍼에 정렬 컨테이너를 쓰지
    // 않는다).
    List<DelayedTimerEntry, DeadlineTraits> expired;
    const uint64_t now = Timer::tickCount();
    {
        SpinlockGuard guard(gLock);
        for (;;) {
            DelayedTimerEntry* front = gList.first();
            if (!front || front->deadlineTick > now) {
                break;
            }
            OrderedList<DelayedTimerEntry, DeadlineTraits>::remove(front);
            expired.pushBack(front);
        }
    }

    while (!expired.empty()) {
        DelayedTimerEntry* front = expired.front();
        List<DelayedTimerEntry, DeadlineTraits>::remove(front);
        front->callback(front->arg);
        GenericSlabAllocator::free(front, sizeof(DelayedTimerEntry));
    }
}

bool DelayedExecutionQueue::hasPending() {
    SpinlockGuard guard(gLock);
    return !gList.empty();
}

}  // namespace kernel
