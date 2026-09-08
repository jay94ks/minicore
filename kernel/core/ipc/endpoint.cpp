// Call/Reply 구현 (docs/spec/ipc.md §3~5). endpoint.hpp 상단 주석 참고.
#include "ipc/endpoint.hpp"

#include <libk/irq_safe.hpp>  // scoped_lock

#include "sched/scheduler.hpp"

namespace ipc {

namespace {

struct resolved_endpoint {
    object::endpoint* ep;
    uint64_t badge;
};

// h가 가리키는 핸들이 유효하고, endpoint 종류이며, required_right를
// 갖는지 확인한다 — objects.md §3의 handle_entry 검사를 ipc.md §6의
// 에러 코드로 옮긴다.
result<resolved_endpoint, ipc_error> resolve_endpoint(object::handle_table& table,
                                                        object::handle h, uint32_t required_right) {
    const object::handle_entry* e = table.debug_entry(h);
    if (e == nullptr || !e->valid) {
        return result<resolved_endpoint, ipc_error>::err(ipc_error::invalid_handle);
    }
    if (e->kind != object::object_kind::endpoint) {
        return result<resolved_endpoint, ipc_error>::err(ipc_error::wrong_object_type);
    }
    if ((e->rights & required_right) == 0) {
        return result<resolved_endpoint, ipc_error>::err(ipc_error::permission_denied);
    }
    return result<resolved_endpoint, ipc_error>::ok(
        resolved_endpoint{static_cast<object::endpoint*>(e->object), e->badge});
}

}  // namespace

result<void, ipc_error> sys_call(object::handle_table& table, object::handle h,
                                  const message& msg_in, message& msg_out) {
    auto resolved = resolve_endpoint(table, h, object::k_right_can_send);
    if (!resolved.is_ok()) {
        return result<void, ipc_error>::err(resolved.error());
    }
    object::endpoint& ep = *resolved.value().ep;
    uint64_t badge = resolved.value().badge;

    object::thread* caller = sched::current();
    caller->ipc.reply_dest = &msg_out;

    object::thread* server = nullptr;
    {
        scoped_lock<spinlock> guard(ep.lock);
        if (!ep.waiting_servers.empty()) {
            server = &ep.waiting_servers.front();
            decltype(ep.waiting_servers)::erase(*server);
        } else {
            caller->ipc.pending_call_msg = &msg_in;
            caller->ipc.pending_call_badge = badge;
            ep.waiting_callers.push_back(*caller);
        }
    }

    if (server != nullptr) {
        // 이미 sys_recv로 대기 중인 서버가 있었다 — 즉시 핸드오프 +
        // 도네이션(ipc.md §5 1단계, ADR-028).
        *server->ipc.recv_dest = msg_in;
        server->ipc.recv_badge = badge;
        server->ipc.reply_target = caller;
        server->ipc.saved_boost_level = server->sched.boost_level;
        server->sched.boost_level = caller->sched.boost_level;
        sched::enqueue(*server);
    }

    // sys_reply(§5 3단계)가 다시 깨울 때까지 블록한다. 깨어난 시점에는
    // msg_out/badge가 이미 채워져 있다.
    sched::block();

    return result<void, ipc_error>::ok();
}

result<uint64_t, ipc_error> sys_recv(object::handle_table& table, object::handle h,
                                      message& msg_out) {
    auto resolved = resolve_endpoint(table, h, object::k_right_can_recv);
    if (!resolved.is_ok()) {
        return result<uint64_t, ipc_error>::err(resolved.error());
    }
    object::endpoint& ep = *resolved.value().ep;

    object::thread* self = sched::current();

    object::thread* caller = nullptr;
    {
        scoped_lock<spinlock> guard(ep.lock);
        if (!ep.waiting_callers.empty()) {
            caller = &ep.waiting_callers.front();
            decltype(ep.waiting_callers)::erase(*caller);
        } else {
            self->ipc.recv_dest = &msg_out;
            ep.waiting_servers.push_back(*self);
        }
    }

    if (caller != nullptr) {
        // 이미 sys_call로 대기 중인 호출자가 있었다 — 즉시 페어링,
        // 도네이션 적용. 이 스레드(서버)는 블록하지 않고 바로 반환한다.
        msg_out = *caller->ipc.pending_call_msg;
        uint64_t badge = caller->ipc.pending_call_badge;
        self->ipc.reply_target = caller;
        self->ipc.saved_boost_level = self->sched.boost_level;
        self->sched.boost_level = caller->sched.boost_level;
        return result<uint64_t, ipc_error>::ok(badge);
    }

    // sys_call이 나를 깨울 때까지 블록한다 — 깨어난 시점에는 msg_out/
    // reply_target/recv_badge가 이미 채워져 있다.
    sched::block();
    return result<uint64_t, ipc_error>::ok(self->ipc.recv_badge);
}

void sys_reply(const message& msg_in) {
    object::thread* self = sched::current();
    object::thread* caller = self->ipc.reply_target;
    if (caller == nullptr) {
        // 대응하는 sys_recv가 없다 — ipc.md §3: 오류가 아니라 아무
        // 동작도 하지 않는다.
        return;
    }
    self->ipc.reply_target = nullptr;

    *caller->ipc.reply_dest = msg_in;

    // 도네이션 복원(ipc.md §5 3단계, ADR-028).
    self->sched.boost_level = self->ipc.saved_boost_level;

    sched::enqueue(*caller);
    // 서버(self)는 블록하지 않고 계속 실행한다.
}

}  // namespace ipc
