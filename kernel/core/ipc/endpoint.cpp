// Call/Reply 구현 (docs/spec/ipc.md §3~5). endpoint.hpp 상단 주석 참고.
#include "ipc/endpoint.hpp"

#include <libk/irq_safe.hpp>  // scoped_lock

#include <mm/page_allocator.hpp>  // mm::k_page_size (페이지 정렬 검사)
#include <mm/phys_map.hpp>        // mm::phys_to_virt(cross-address-space 메시지 번역)

#include "sched/scheduler.hpp"

// M13(ADR-151) — kernel/core는 arch 헤더를 포함하지 않는다(ADR-002)
// 는 규칙을 지키면서 "이 유저 가상주소가 어느 물리 프레임에 매핑돼
// 있는가"를 물어야 해서, core/sched/scheduler.cpp의 arch_context_switch
// 등과 같은 패턴(extern "C" 훅, 정의는 arch가 담당)을 쓴다 — 실제
// 정의는 kernel/arch/x86_64/page_table.cpp의 query_page()를 감싼다.
extern "C" bool arch_translate_user_page(uint64_t page_table_root, uint64_t vaddr,
                                          uint64_t* out_phys);

namespace ipc {

namespace {

struct resolved_endpoint {
    object::endpoint* ep;
    uint64_t badge;
};

// space==nullptr(커널 스레드, M6~M8 데모의 전제 — owner_space가 없는
// 스레드는 애초에 "유저 가상주소 번역"이 필요 없는 커널 컨텍스트
// 포인터만 쓴다)이면 vaddr을 그대로 커널 포인터로 취급한다(기존
// 단순화 그대로 보존). space!=nullptr이면 그 주소공간의
// page_table_root로 vaddr을 물리 프레임으로 번역해
// mm::phys_to_virt로 커널이 역참조 가능한 포인터를 얻는다 — 페이지
// 경계를 넘는 범위는 페이지 단위로 나눠 처리한다.
bool copy_user_bytes(object::address_space* space, uint64_t vaddr, void* kernel_buf,
                      uint64_t len, bool from_user) {
    if (space == nullptr) {
        if (from_user) {
            __builtin_memcpy(kernel_buf, reinterpret_cast<const void*>(vaddr), len);
        } else {
            __builtin_memcpy(reinterpret_cast<void*>(vaddr), kernel_buf, len);
        }
        return true;
    }

    uint64_t remaining = len;
    uint64_t cur_vaddr = vaddr;
    auto* cur_buf = static_cast<uint8_t*>(kernel_buf);
    while (remaining > 0) {
        uint64_t page_base = cur_vaddr & ~static_cast<uint64_t>(mm::k_page_size - 1);
        uint64_t offset_in_page = cur_vaddr - page_base;
        uint64_t chunk = mm::k_page_size - offset_in_page;
        if (chunk > remaining) {
            chunk = remaining;
        }

        uint64_t phys = 0;
        if (!arch_translate_user_page(space->page_table_root, page_base, &phys)) {
            return false;
        }
        auto* page_virt = static_cast<uint8_t*>(mm::phys_to_virt(phys));
        if (from_user) {
            __builtin_memcpy(cur_buf, page_virt + offset_in_page, chunk);
        } else {
            __builtin_memcpy(page_virt + offset_in_page, cur_buf, chunk);
        }

        cur_vaddr += chunk;
        cur_buf += chunk;
        remaining -= chunk;
    }
    return true;
}

bool copy_from_user(object::address_space* space, uint64_t vaddr, void* dst, uint64_t len) {
    return copy_user_bytes(space, vaddr, dst, len, /*from_user=*/true);
}

bool copy_to_user(object::address_space* space, uint64_t vaddr, const void* src, uint64_t len) {
    return copy_user_bytes(space, vaddr, const_cast<void*>(src), len, /*from_user=*/false);
}

// M8부터 유저 스레드만 handles(handle_table*)를 갖는다(kernel_objects.hpp
// 참고) — 커널 스레드(M6/M7 데모)는 nullptr이라, 그 경우 sys_call/
// sys_recv 호출자가 넘긴 table(양쪽이 같은 g_ipc_table을 공유하는
// 기존 전제)로 그대로 되돌아간다. 이래야 기존 커널 스레드 IPC 데모의
// 동작이 이 리팩터 이후에도 완전히 그대로 유지된다.
object::handle_table& table_for_thread(object::thread* t, object::handle_table& fallback) {
    return (t->handles != nullptr) ? *t->handles : fallback;
}

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

// src_vaddr(송신자 주소공간에 있는 메시지)를 dst_vaddr(수신자 주소공간에
// 있는 메시지)로 옮긴다. 둘 다 "그 메시지가 있는 스레드의 owner_space"
// (커널 스레드면 nullptr)로 번역해 읽고 쓴다(ADR-151) — 이래야 서로
// 다른 유저 프로세스 사이의 IPC(M13부터 실제로 등장)에서도 label/regs/
// page_count/handle_count가 올바르게 전달된다. dst의 pages[]는 호출
// 시점에 "수신자가 미리 지정한 목적지 버퍼"를 담고 있어야 한다
// (message.hpp 방향 규약)라, 먼저 dst의 기존 내용을 읽어 온 뒤 그
// 위에 덮어써야 한다.
//
// **남은 단순화**(endpoint.hpp 상단 주석 참고): page_descriptor가
// 가리키는 실제 데이터 버퍼(in_pd.vaddr/dst.pages[i].vaddr)는 여전히
// "그 vaddr을 쓰는 스레드와 이 함수를 실행하는 스레드가 같은
// 주소공간"이라는 전제로 직접 역참조한다 — M13은 이 경로를 아예
// 쓰지 않으므로(전부 regs[]만 사용) 지금 고치지 않는다.
result<void, ipc_error> deliver_message(uint64_t src_vaddr, object::address_space* src_space,
                                         uint64_t dst_vaddr, object::address_space* dst_space,
                                         object::handle_table& src_table,
                                         object::handle_table& dst_table) {
    message src{};
    if (!copy_from_user(src_space, src_vaddr, &src, sizeof(message))) {
        return result<void, ipc_error>::err(ipc_error::page_not_mapped);
    }
    message dst{};
    if (!copy_from_user(dst_space, dst_vaddr, &dst, sizeof(message))) {
        return result<void, ipc_error>::err(ipc_error::page_not_mapped);
    }

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

    if (!copy_to_user(dst_space, dst_vaddr, &dst, sizeof(message))) {
        return result<void, ipc_error>::err(ipc_error::page_not_mapped);
    }
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
        auto xfer = deliver_message(reinterpret_cast<uint64_t>(&msg_in), caller->owner_space,
                                     reinterpret_cast<uint64_t>(server->ipc.recv_dest),
                                     server->owner_space, table, table_for_thread(server, table));
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
        auto xfer = deliver_message(reinterpret_cast<uint64_t>(caller->ipc.pending_call_msg),
                                     caller->owner_space, reinterpret_cast<uint64_t>(&msg_out),
                                     self->owner_space, table_for_thread(caller, table), table);
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

result<void, ipc_error> sys_reply(object::handle_table& table, const message& msg_in) {
    object::thread* self = sched::current();
    object::thread* caller = self->ipc.reply_target;
    if (caller == nullptr) {
        // 대응하는 sys_recv가 없다 — ipc.md §3: 오류가 아니라 아무
        // 동작도 하지 않는다.
        return result<void, ipc_error>::ok();
    }
    self->ipc.reply_target = nullptr;

    // M13(ADR-151) — 이제 handles[]까지 옮긴다(ADR-018이 요구하는
    // "VFS가 open() 응답에서 FS 서버 핸들을 위임"이 이 방향이라야
    // 가능하다). deliver_message를 그대로 재사용한다 — src(this
    // thread가 채운 msg_in)와 dst(caller의 reply_dest)가 서로 다른
    // 주소공간이라도 안전하다.
    auto xfer = deliver_message(reinterpret_cast<uint64_t>(&msg_in), self->owner_space,
                                 reinterpret_cast<uint64_t>(caller->ipc.reply_dest),
                                 caller->owner_space, table_for_thread(self, table),
                                 table_for_thread(caller, table));

    // 도네이션 복원(ipc.md §5 3단계, ADR-028) — 전달 성공/실패와
    // 무관하게 항상 되돌린다(이 스레드가 계속 그 우선순위로 도는 것을
    // 막아야 한다).
    self->sched.boost_level = self->ipc.saved_boost_level;

    // ipc.md §3 — sys_reply는 블록하지 않는다: 전달이 실패해도(예:
    // caller가 page_count>0인데 목적지를 안 채워 둔 경우) caller를
    // 영원히 블록된 채로 두지 않고 label/regs 없이라도 깨운다 —
    // deliver_message가 실패해도 caller.reply_dest에는 아무것도
    // 쓰이지 않았을 뿐, caller는 그대로 깨어나 자신의 msg_out을
    // (호출 전 상태 그대로) 관찰한다.
    sched::enqueue(*caller);
    // 서버(self)는 블록하지 않고 계속 실행한다.
    return xfer;
}

}  // namespace ipc
