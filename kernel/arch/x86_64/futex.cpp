#include "futex.hpp"

#include <k/irq_safe.hpp>  // scoped_lock

#include <object/kernel_objects.hpp>
#include <sched/scheduler.hpp>

namespace kern::arch::x86_64 {

futex_error futex_wait(uint64_t uaddr, uint32_t expected) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return futex_error::not_a_user_process;
    }
    kern::object::address_space& space = *self->owner_space;

    {
        // endpoint.cpp의 sys_call/sys_recv와 완전히 같은 순서 — 락을
        // 잡은 채로 "값 확인 + 대기열에 자신을 넣기"까지 원자적으로
        // 마친 뒤, 락을 풀고서야 block()한다(그 사이의 아주 좁은 경합
        // 창은 기존 IPC 대기열도 이미 감수하는 것과 같다, OPEN-68
        // 참고 — 이번에 새로 만들지 않는다).
        scoped_lock<spinlock> guard(space.futex_lock);
        const auto* val_ptr = reinterpret_cast<const uint32_t*>(uaddr);
        if (*val_ptr != expected) {
            return futex_error::value_mismatch;
        }
        self->futex_wait_uaddr = uaddr;
        space.futex_waiters.push_back(*self);
    }

    kern::sched::block();
    return futex_error::ok;
}

uint32_t futex_wake(uint64_t uaddr, uint32_t max_count) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return 0;
    }
    kern::object::address_space& space = *self->owner_space;

    uint32_t woken = 0;
    scoped_lock<spinlock> guard(space.futex_lock);
    auto it = space.futex_waiters.begin();
    while (it != space.futex_waiters.end() && woken < max_count) {
        kern::object::thread& candidate = *it;
        ++it;  // erase가 candidate의 훅을 끊기 전에 다음 노드로 미리 옮겨 둔다.
        if (candidate.futex_wait_uaddr == uaddr) {
            decltype(space.futex_waiters)::erase(candidate);
            kern::sched::enqueue(candidate);
            ++woken;
        }
    }
    return woken;
}

}  // namespace kern::arch::x86_64
