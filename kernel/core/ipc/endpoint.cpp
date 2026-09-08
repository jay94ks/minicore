// Call/Reply 구현 (docs/spec/ipc.md §3~5). endpoint.hpp 상단 주석 참고.
#include "ipc/endpoint.hpp"

#include <libk/irq_safe.hpp>  // scoped_lock

#include <mm/page_allocator.hpp>  // mm::k_page_size (페이지 정렬 검사)

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

// src(송신자가 채운 메시지)를 dst(수신자가 받을 메시지)로 옮긴다.
// dst.pages[]는 호출 시점에 "수신자가 미리 지정한 목적지 버퍼"를
// 담고 있어야 한다(message.hpp 방향 규약) — 이 함수가 그 값을 읽어서
// 쓰고, 끝나면 실제로 전달된 서술자로 덮어쓴다.
//
// 알려진 단순화: 이 마일스톤(M7)의 스레드는 전부 커널 자신의 주소
// 공간(부팅 때 만든 것 하나)에서 돈다 — 서로 다른 프로세스 주소공간이
// 아직 없다(M8 이후). 그래서 vaddr을 "그 스레드가 속한 주소공간의
// 페이지테이블로 찾아야 할 가상주소"가 아니라 커널 자신이 이미
// 역참조 가능한 포인터 값으로 바로 쓴다 — 실제 물리 복사(ADR-015)를
// 하지만, 서로 다른 주소공간 간 변환(ADR-013의 "커널 자동 번역")은
// 아직 하지 않는다. handle 위임도 마찬가지로 src_table과 dst_table이
// 지금은 같은 handle_table(공유 "커널 컨텍스트")일 수 있다 — M4/M6
// 데모와 같은 단순화.
result<void, ipc_error> deliver_message(const message& src, message& dst,
                                         object::handle_table& src_table,
                                         object::handle_table& dst_table) {
    dst.label = src.label;
    for (size_t i = 0; i < k_message_registers; ++i) {
        dst.regs[i] = src.regs[i];
    }

    if (src.page_count > k_max_page_descriptors) {
        return result<void, ipc_error>::err(ipc_error::message_too_large);
    }
    for (uint32_t i = 0; i < src.page_count; ++i) {
        const page_descriptor& in_pd = src.pages[i];
        if (in_pd.mode != transfer_mode::copy) {
            // move/map은 이 마일스톤 범위 밖(계획 문서 그대로, ADR-015).
            return result<void, ipc_error>::err(ipc_error::permission_denied);
        }
        if (in_pd.length == 0 || (in_pd.vaddr % mm::k_page_size) != 0 ||
            (in_pd.length % mm::k_page_size) != 0) {
            // 정렬 위반은 호출자 버그다(ipc.md §4 "페이지 정렬") —
            // 조용히 틀린 값을 받아들이지 않는다.
            LIBK_PANIC("ipc: page_descriptor not page-aligned");
        }
        if (i >= dst.page_count || dst.pages[i].length < in_pd.length) {
            // 수신자가 이 인덱스에 충분한 목적지 버퍼를 준비해 두지 않았다.
            return result<void, ipc_error>::err(ipc_error::page_not_mapped);
        }
        __builtin_memcpy(reinterpret_cast<void*>(dst.pages[i].vaddr),
                         reinterpret_cast<const void*>(in_pd.vaddr), in_pd.length);
    }
    dst.page_count = src.page_count;

    if (src.handle_count > k_max_handle_transfers) {
        return result<void, ipc_error>::err(ipc_error::message_too_large);
    }
    for (uint32_t i = 0; i < src.handle_count; ++i) {
        auto proxy = src_table.create_proxy(src.handles[i].src_handle, src.handles[i].rights_mask,
                                             dst_table, /*badge_override=*/0,
                                             /*has_badge_override=*/false);
        if (!proxy.is_ok()) {
            // objects.md §4 3단계와 같은 정신 — 이 핸들 하나만 부분
            // 실패로 취급하고(INVALID_HANDLE로 표시) 나머지는 계속
            // 진행한다. 정확한 부분 실패 보고 형식은 스펙도 "구현
            // 시 정한다"고 미뤄둔 부분이다(objects.md "아직 정하지
            // 않은 것").
            dst.handles[i].src_handle = object::k_invalid_handle;
            dst.handles[i].rights_mask = 0;
            continue;
        }
        dst.handles[i].src_handle = proxy.value();  // in/out(§4 7단계): 새 핸들 번호로 덮어씀.
        dst.handles[i].rights_mask = src.handles[i].rights_mask;
    }
    dst.handle_count = src.handle_count;

    return result<void, ipc_error>::ok();
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
        // 이미 sys_recv로 대기 중인 서버가 있었다 — 즉시 핸드오프.
        auto xfer = deliver_message(msg_in, *server->ipc.recv_dest, table, table);
        if (!xfer.is_ok()) {
            // 아무 일도 없었던 것처럼 서버를 다시 대기열에 넣고, 호출자에게는
            // 즉시(블록 없이) 에러를 반환한다.
            scoped_lock<spinlock> guard(ep.lock);
            ep.waiting_servers.push_back(*server);
            return result<void, ipc_error>::err(xfer.error());
        }

        // 도네이션(ipc.md §5 1단계, ADR-028).
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
        // 이미 sys_call로 대기 중인 호출자가 있었다 — 즉시 페어링.
        auto xfer = deliver_message(*caller->ipc.pending_call_msg, msg_out, table, table);
        if (!xfer.is_ok()) {
            scoped_lock<spinlock> guard(ep.lock);
            ep.waiting_callers.push_back(*caller);
            return result<uint64_t, ipc_error>::err(xfer.error());
        }

        // 도네이션 적용. 이 스레드(서버)는 블록하지 않고 바로 반환한다.
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

    // label+regs만 전달한다(endpoint.hpp 상단 "알려진 단순화").
    caller->ipc.reply_dest->label = msg_in.label;
    for (size_t i = 0; i < k_message_registers; ++i) {
        caller->ipc.reply_dest->regs[i] = msg_in.regs[i];
    }

    // 도네이션 복원(ipc.md §5 3단계, ADR-028).
    self->sched.boost_level = self->ipc.saved_boost_level;

    sched::enqueue(*caller);
    // 서버(self)는 블록하지 않고 계속 실행한다.
}

}  // namespace ipc
