// Notification 구현 (docs/spec/ipc.md §7). notification.hpp 상단 주석
// 참고.
#include "ipc/notification.hpp"

#include <libk/irq_safe.hpp>  // scoped_lock

#include "sched/scheduler.hpp"

namespace kern::ipc {

namespace {

result<kern::object::notification*, ipc_error> resolve_notification(kern::object::handle_table& table,
                                                                 kern::object::handle h) {
    const kern::object::handle_entry* e = table.debug_entry(h);
    if (e == nullptr || !e->valid) {
        return result<kern::object::notification*, ipc_error>::err(ipc_error::invalid_handle);
    }
    if (e->kind != kern::object::object_kind::notification) {
        return result<kern::object::notification*, ipc_error>::err(ipc_error::wrong_object_type);
    }
    return result<kern::object::notification*, ipc_error>::ok(
        static_cast<kern::object::notification*>(e->object));
}

}  // namespace

result<void, ipc_error> sys_notify(kern::object::handle_table& table, kern::object::handle h, uint64_t bits) {
    auto resolved = resolve_notification(table, h);
    if (!resolved.is_ok()) {
        return result<void, ipc_error>::err(resolved.error());
    }
    kern::object::notification& n = *resolved.value();

    // OR — atomic<T>에는 fetch_or가 없다(ADR-072: 필요해지는 조합만
    // 추가), CAS 루프로 구현한다.
    uint64_t expected = n.bits.load_relaxed();
    while (!n.bits.compare_exchange_strong_acq_rel(expected, expected | bits)) {
        // expected가 관측된 현재값으로 갱신됐다(표준 CAS 관용) — 재시도.
    }

    kern::object::thread* to_wake = nullptr;
    {
        scoped_lock<spinlock> guard(n.lock);
        if (n.waiter != nullptr) {
            to_wake = n.waiter;
            n.waiter = nullptr;
        }
    }
    if (to_wake != nullptr) {
        kern::sched::enqueue(*to_wake);
    }

    return result<void, ipc_error>::ok();
}

result<uint64_t, ipc_error> sys_wait(kern::object::handle_table& table, kern::object::handle h) {
    auto resolved = resolve_notification(table, h);
    if (!resolved.is_ok()) {
        return result<uint64_t, ipc_error>::err(resolved.error());
    }
    kern::object::notification& n = *resolved.value();

    while (true) {
        uint64_t current = n.bits.load_acquire();
        if (current != 0) {
            uint64_t expected = current;
            if (n.bits.compare_exchange_strong_acq_rel(expected, 0)) {
                return result<uint64_t, ipc_error>::ok(expected);
            }
            continue;  // 다른 경합자가 먼저 채갔다 — 다시 읽는다.
        }

        {
            scoped_lock<spinlock> guard(n.lock);
            // 락을 잡은 채로 다시 확인한다 — sys_notify가 그 사이 비트를
            // 세팅했을 수 있다(이 마일스톤은 단일 코어 협조적 스케줄러라
            // 실제로 동시 발생하지는 않지만, 다중 코어를 전제로 한
            // 자료구조 설계 원칙(ADR-035)에 맞춰 방어적으로 재확인한다).
            if (n.bits.load_acquire() != 0) {
                continue;
            }
            n.waiter = kern::sched::current();
        }
        kern::sched::block();
        // sys_notify가 깨웠다 — 위 루프 처음으로 돌아가 비트를 다시 읽는다.
    }
}

}  // namespace kern::ipc
