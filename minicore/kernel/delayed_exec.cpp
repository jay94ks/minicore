#include "delayed_exec.h"

#include "libkenv/spinlock.h"
#include "libkmm/slab.h"
#include "timer.h"

namespace {

kernel::DelayedTimerEntry* gHead = nullptr;
kernel::Spinlock gLock;
kernel::uint64_t gNextToken = 1;  // 0은 "실패/무효 토큰" 전용(schedule() 주석 참고)

}  // namespace

namespace kernel {

void DelayedExecutionQueue::init() {
    SpinlockGuard guard(gLock);
    gHead = nullptr;
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

    SpinlockGuard guard(gLock);
    entry->token = gNextToken++;
    entry->next = gHead;
    gHead = entry;
    return entry->token;
}

bool DelayedExecutionQueue::cancel(uint64_t token) {
    SpinlockGuard guard(gLock);
    DelayedTimerEntry* prev = nullptr;
    DelayedTimerEntry* cur = gHead;
    while (cur) {
        if (cur->token == token) {
            if (prev) {
                prev->next = cur->next;
            } else {
                gHead = cur->next;
            }
            GenericSlabAllocator::free(cur, sizeof(DelayedTimerEntry));
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

void DelayedExecutionQueue::pump() {
    // 만료 항목을 리스트에서 먼저 전부 떼어낸 뒤(락 보호 구간은 이
    // 분리 작업까지만), 콜백은 락 밖에서 실행한다 - 콜백이 다시
    // schedule()/cancel()을 호출해도(재진입) 같은 락을 다시 잡다가
    // 자기 자신과 데드락에 빠지지 않는다.
    DelayedTimerEntry* expiredHead = nullptr;
    const uint64_t now = Timer::tickCount();
    {
        SpinlockGuard guard(gLock);
        DelayedTimerEntry* prev = nullptr;
        DelayedTimerEntry* cur = gHead;
        while (cur) {
            DelayedTimerEntry* next = cur->next;
            if (cur->deadlineTick <= now) {
                if (prev) {
                    prev->next = next;
                } else {
                    gHead = next;
                }
                cur->next = expiredHead;
                expiredHead = cur;
            } else {
                prev = cur;
            }
            cur = next;
        }
    }

    while (expiredHead) {
        DelayedTimerEntry* next = expiredHead->next;
        expiredHead->callback(expiredHead->arg);
        GenericSlabAllocator::free(expiredHead, sizeof(DelayedTimerEntry));
        expiredHead = next;
    }
}

bool DelayedExecutionQueue::hasPending() {
    SpinlockGuard guard(gLock);
    return gHead != nullptr;
}

}  // namespace kernel
