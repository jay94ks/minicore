// 스케줄러 골격 구현 (docs/spec/scheduler.md §1~3). scheduler.hpp 상단
// 주석 참고 — 협조적 전환만 다룬다.
#include "sched/scheduler.hpp"

#include <new>

#include <libk/irq_safe.hpp>  // scoped_lock
#include <libk/panic.hpp>

#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>
#include <mm/slab.hpp>

// arch::context_switch(&old->context_rsp, new->context_rsp) — 현재
// 레지스터 상태를 콜리 세이브 레지스터 + 복귀 주소만 스택에 남기고
// old_rsp_out에 저장한 뒤, new_rsp로 스택을 바꿔 그 지점부터 이어
// 실행한다(classic swtch 기법). 커널 arch 계층만 정의를 제공한다
// (ADR-002 HAL 경계 — 이 파일은 x86_64 어셈블리를 전혀 모른다).
extern "C" void arch_context_switch(uint64_t* old_rsp_out, uint64_t new_rsp);

namespace sched {

namespace {

run_queue* g_run_queues = nullptr;
uint32_t g_node_count = 0;
object::thread* g_current = nullptr;

// start() 호출 시점의 kernel_main 실행 흐름을 "버리는" 곳 — 다시 읽지
// 않는다. arch_context_switch는 첫 인자로 반드시 유효한 쓰기 위치를
// 요구하므로 자리만 마련해 둔다.
uint64_t g_bootstrap_discard_rsp = 0;

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
    arch_context_switch(&g_bootstrap_discard_rsp, next->context_rsp);
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
    arch_context_switch(&prev->context_rsp, next->context_rsp);
    // arch_context_switch에서 돌아왔다는 것은 prev가 다시 스케줄되어
    // 이 지점부터 재개됐다는 뜻이다.
}

object::thread* current() { return g_current; }

}  // namespace sched
