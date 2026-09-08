// 스케줄러 골격 구현 (docs/spec/scheduler.md §1~3). scheduler.hpp 상단
// 주석 참고 — 협조적 전환만 다룬다.
#include "sched/scheduler.hpp"

#include <new>

#include <libk/irq_safe.hpp>  // scoped_lock
#include <libk/panic.hpp>

#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>
#include <mm/slab.hpp>

// arch::context_switch(&old->context_rsp, new->context_rsp, new_pml4_phys) —
// 현재 레지스터 상태를 콜리 세이브 레지스터 + 복귀 주소만 스택에 남기고
// old_rsp_out에 저장한 뒤, new_rsp로 스택을 바꿔 그 지점부터 이어
// 실행한다(classic swtch 기법). 커널 arch 계층만 정의를 제공한다
// (ADR-002 HAL 경계 — 이 파일은 x86_64 어셈블리를 전혀 모른다).
// new_pml4_phys(M8 추가): 0이면 CR3 유지, 0이 아니면 그 물리주소로
// 전환한다(context_switch.S 상단 주석).
extern "C" void arch_context_switch(uint64_t* old_rsp_out, uint64_t new_rsp,
                                     uint64_t new_pml4_phys);

// M8 — 새로 만든 유저 스레드가 "처음" 스케줄될 때 진입하는 자리
// (create_kernel_thread의 entry 자리를 유저 스레드는 이걸로 대신한다).
// arch 계층이 IRETQ로 실제 유저모드 진입을 수행한다(usermode.S/user_thread.cpp).
extern "C" [[noreturn]] void arch_user_thread_trampoline();

// M8(exit() 도입과 함께 추가) — 이 코어에서 더 이상 아무도 runnable하지
// 않을 때 최종적으로 멈추는 자리. HLT는 arch 명령이라 여기서 직접 쓸 수
// 없다(ADR-002 HAL 경계) — arch 계층이 제공한다(idle.S).
extern "C" [[noreturn]] void arch_idle_halt();

namespace sched {

namespace {

run_queue* g_run_queues = nullptr;
uint32_t g_node_count = 0;
object::thread* g_current = nullptr;

// start() 호출 시점의 kernel_main 실행 흐름을 "버리는" 곳 — 다시 읽지
// 않는다. arch_context_switch는 첫 인자로 반드시 유효한 쓰기 위치를
// 요구하므로 자리만 마련해 둔다.
uint64_t g_bootstrap_discard_rsp = 0;

// M8 — next가 유저 스레드(owner_space가 있음)면 그 주소공간의 PML4로
// CR3를 전환해야 한다. 커널 스레드는 owner_space == nullptr이라 항상
// 0(= CR3 유지)을 반환한다 — arch_context_switch가 0을 "건드리지 않음"
// 신호로 해석한다(context_switch.S).
uint64_t next_pml4_phys(const object::thread& next) {
    return next.owner_space != nullptr ? next.owner_space->page_table_root : 0;
}

object::thread* pick_next_locked(run_queue& rq) {
    if (!rq.kernel_band.empty()) {
        object::thread& t = rq.kernel_band.front();
        decltype(rq.kernel_band)::erase(t);
        return &t;
    }
    if (!rq.user_band.empty()) {
        object::thread& t = rq.user_band.front();
        decltype(rq.user_band)::erase(t);
        return &t;
    }
    return nullptr;
}

}  // namespace

void init() {
    g_node_count = mm::node_count();
    if (g_node_count > mm::k_max_numa_nodes) {
        g_node_count = mm::k_max_numa_nodes;
    }
    if (g_node_count == 0) {
        g_node_count = 1;
    }

    // run_queue 배열도 handle_table(kernel-bootstrap-m4.md)과 같은 이유로
    // 전역에 두지 않고 여기서 mm 위에 만든다.
    auto page = mm::alloc_pages(0, 0);
    if (!page.is_ok()) {
        LIBK_PANIC("sched::init: no memory for run_queue array");
    }
    g_run_queues = static_cast<run_queue*>(mm::phys_to_virt(page.value()));
    for (uint32_t i = 0; i < g_node_count; ++i) {
        new (&g_run_queues[i]) run_queue();
    }
}

object::thread* create_kernel_thread(void (*entry)(), object::priority_band band,
                                      uint32_t preferred_node) {
    void* mem = mm::slab_alloc(sizeof(object::thread));
    if (mem == nullptr) {
        return nullptr;
    }
    auto* t = new (mem) object::thread();
    t->sched.band = band;
    t->sched.preferred_node = preferred_node;

    constexpr uint32_t k_stack_order = 2;  // 16KiB (scheduler.hpp 상단 주석)
    auto stack_page = mm::alloc_pages(k_stack_order, preferred_node);
    if (!stack_page.is_ok()) {
        mm::slab_free(t, sizeof(object::thread));
        return nullptr;
    }

    auto* stack_base = static_cast<uint8_t*>(mm::phys_to_virt(stack_page.value()));
    uint8_t* stack_top = stack_base + (static_cast<uint64_t>(mm::k_page_size) << k_stack_order);

    // arch_context_switch가 기대하는 초기 스택 레이아웃을 손으로 만든다
    // — "이 스레드는 예전에 한 번 context_switch를 통해 잠들었었고,
    // 그때 콜리세이브 레지스터 6개를 push해 뒀다"는 상태를 흉내낸다.
    // 실제 값은 의미 없으므로(첫 실행이니까) 전부 0으로 채우고, 맨
    // 밑에 entry 주소만 진짜로 넣는다 — arch_context_switch 끝의 ret가
    // 이걸 복귀 주소로 착각해 entry로 뛰어든다.
    uint64_t* sp = reinterpret_cast<uint64_t*>(stack_top);
    *(--sp) = reinterpret_cast<uint64_t>(entry);
    *(--sp) = 0;  // rbp
    *(--sp) = 0;  // rbx
    *(--sp) = 0;  // r12
    *(--sp) = 0;  // r13
    *(--sp) = 0;  // r14
    *(--sp) = 0;  // r15
    t->context_rsp = reinterpret_cast<uint64_t>(sp);

    return t;
}

object::thread* create_user_thread(uint64_t entry_rip, uint64_t user_rsp, uint64_t arg0,
                                    object::address_space* space, object::handle_table* handles) {
    void* mem = mm::slab_alloc(sizeof(object::thread));
    if (mem == nullptr) {
        return nullptr;
    }
    auto* t = new (mem) object::thread();
    t->sched.band = object::priority_band::user;
    t->sched.preferred_node = 0;
    t->owner_space = space;
    t->handles = handles;
    t->user_entry_rip = entry_rip;
    t->user_rsp = user_rsp;
    t->user_arg0 = arg0;

    // 커널 스택 — create_kernel_thread와 같은 크기(16KiB). 유저
    // 스레드에게는 두 가지 역할: (1) 지금 여기서 만드는 "최초 진입"
    // 컨텍스트가 쓰는 스택, (2) 이후 이 스레드가 syscall로 커널에
    // 들어올 때 쓰는 스택(syscall.cpp의 g_syscall_kernel_rsp가
    // arch_user_thread_trampoline 실행 시점에 이 스택 top을 기억해
    // 둔다) — 유저 스택(user_rsp)과는 별개다.
    constexpr uint32_t k_kstack_order = 2;
    auto stack_page = mm::alloc_pages(k_kstack_order, 0);
    if (!stack_page.is_ok()) {
        mm::slab_free(t, sizeof(object::thread));
        return nullptr;
    }

    auto* stack_base = static_cast<uint8_t*>(mm::phys_to_virt(stack_page.value()));
    uint8_t* stack_top = stack_base + (static_cast<uint64_t>(mm::k_page_size) << k_kstack_order);

    // create_kernel_thread와 같은 손짜기 초기 스택 — 복귀 주소만
    // arch_user_thread_trampoline으로 바꿨다.
    uint64_t* sp = reinterpret_cast<uint64_t*>(stack_top);
    *(--sp) = reinterpret_cast<uint64_t>(&arch_user_thread_trampoline);
    *(--sp) = 0;  // rbp
    *(--sp) = 0;  // rbx
    *(--sp) = 0;  // r12
    *(--sp) = 0;  // r13
    *(--sp) = 0;  // r14
    *(--sp) = 0;  // r15
    t->context_rsp = reinterpret_cast<uint64_t>(sp);

    return t;
}

void enqueue(object::thread& t) {
    run_queue& rq = g_run_queues[t.sched.preferred_node % g_node_count];
    scoped_lock<spinlock> guard(rq.lock);
    if (t.sched.band == object::priority_band::kernel) {
        rq.kernel_band.push_back(t);
    } else {
        rq.user_band.push_back(t);
    }
}

void start() {
    // M1~M8은 노드 1개(ADR-035) — 현재 코어가 보는 run_queue는 항상
    // 노드 0이다(M9 이후 실제 다중 코어가 붙으면 "이 코어의 노드"를
    // 조회하는 절차가 필요해진다).
    run_queue& rq = g_run_queues[0];
    object::thread* next;
    {
        scoped_lock<spinlock> guard(rq.lock);
        next = pick_next_locked(rq);
    }
    if (next == nullptr) {
        LIBK_PANIC("sched::start: no runnable thread");
    }

    g_current = next;
    arch_context_switch(&g_bootstrap_discard_rsp, next->context_rsp, next_pml4_phys(*next));
    __builtin_unreachable();
}

void yield() {
    object::thread* prev = g_current;

    run_queue& rq = g_run_queues[0];
    object::thread* next;
    {
        scoped_lock<spinlock> guard(rq.lock);
        next = pick_next_locked(rq);
    }
    if (next == nullptr) {
        // 다른 runnable 스레드가 없다 — 계속 실행한다.
        return;
    }

    enqueue(*prev);
    g_current = next;
    arch_context_switch(&prev->context_rsp, next->context_rsp, next_pml4_phys(*next));
    // arch_context_switch에서 돌아왔다는 것은 prev가 다시 스케줄되어
    // 이 지점부터 재개됐다는 뜻이다.
}

void block() {
    object::thread* prev = g_current;

    run_queue& rq = g_run_queues[0];
    object::thread* next;
    {
        scoped_lock<spinlock> guard(rq.lock);
        next = pick_next_locked(rq);
    }
    if (next == nullptr) {
        LIBK_PANIC("sched::block: no runnable thread (deadlock)");
    }

    // yield()와 달리 prev를 다시 enqueue하지 않는다 — 호출자(예: M6의
    // sys_call/sys_recv)가 prev를 이미 다른 대기열(endpoint의
    // waiting_callers/waiting_servers)에 넣어 뒀거나, 나중에 명시적으로
    // sched::enqueue()할 책임을 진다.
    g_current = next;
    arch_context_switch(&prev->context_rsp, next->context_rsp, next_pml4_phys(*next));
}

[[noreturn]] void exit() {
    // prev(이전 g_current) 자체는 이 함수 안에서 다시 쓸 일이 없다 —
    // block()과 달리 그 context_rsp를 아무도 저장/복원하지 않는다
    // (scheduler.hpp exit() 주석: 다시 스케줄되지 않음이 핵심 보장).
    run_queue& rq = g_run_queues[0];
    object::thread* next;
    {
        scoped_lock<spinlock> guard(rq.lock);
        next = pick_next_locked(rq);
    }
    if (next == nullptr) {
        // 이 코어에서 더 이상 아무도 runnable하지 않다 — 진짜로 멈춘다.
        // prev의 context_rsp는 이제 아무도 다시 읽지 않는다(scheduler.hpp
        // exit() 주석 — 다시 스케줄되지 않음이 이 함수의 핵심 보장이다).
        arch_idle_halt();
    }

    g_current = next;
    // prev를 다시 enqueue하지 않는다 — block()과 같은 메커니즘이지만,
    // block()과 달리 그 무엇도 나중에 prev를 깨우지 않는다(어떤 대기열
    // 에도 prev를 넣어 두지 않았다) — 이 스레드는 여기서 영구히 끝난다.
    uint64_t discard_rsp;
    arch_context_switch(&discard_rsp, next->context_rsp, next_pml4_phys(*next));
    __builtin_unreachable();
}

object::thread* current() { return g_current; }

}  // namespace sched
