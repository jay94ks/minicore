// 스케줄러 골격 구현 (docs/spec/scheduler.md §1~3). scheduler.hpp 상단
// 주석 참고 — 협조적 전환만 다룬다.
#include "sched/scheduler.hpp"

#include <new>

#include <klog.hpp>
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
// FPU(M9 ADR-127 → M11b ADR-133): M9 시절엔 이 함수가 매 스위치마다
// 두 스레드의 fxsave_area를 받아 무조건 FXSAVE/FXRSTOR했다(eager).
// M11b부터는 이 함수가 FPU 상태를 전혀 건드리지 않는다(CR0.TS만
// 세운다) — 실제 저장/복원은 #NM 트랩(fpu.cpp)이 필요한 순간에만
// 한다. 그래서 이 선언에는 더 이상 fxsave 포인터가 없다.
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

// M11(ADR-036/053) — "지금 이 코드를 실행 중인 코어가 속한 NUMA 노드
// 번호". arch 계층(x86_64: smp.cpp)이 LAPIC ID를 읽어 SRAT가 준 표를
// 찾아본다 — kernel/core/sched는 APIC/LAPIC의 존재를 몰라도 된다
// (ADR-002 HAL 경계, arch_context_switch와 같은 관례).
extern "C" uint32_t arch_current_node_id();

// M11b(ADR-133 §결정3) — 영구 종료하는 스레드가 어느 코어의 FPU
// 소유자였다면 그 기록을 지운다. kernel/core/sched는 "FPU 소유자"라는
// 개념 자체를 몰라도 된다 — arch 계층(x86_64: fpu.cpp)이 이 스레드
// 포인터가 자기 내부 표에 남아 있는지만 확인한다.
extern "C" void arch_fpu_thread_exiting(kern::object::thread* t);

// M12(ADR-142) — sys_fork의 자식이 "처음" 스케줄될 때 진입하는 자리.
// arch_user_thread_trampoline과 같은 역할이지만, 진입점이 고정된
// ELF entry가 아니라 "부모가 SYSCALL을 실행한 순간으로 복귀"다
// (syscall_entry.S 상단 주석, create_forked_thread 참고).
extern "C" [[noreturn]] void arch_fork_child_resume();

// M14(ADR-154, OPEN-58 해소) — next가 sys_io_activate로 활성화해 둔
// I/O 포트 범위(kern::object::thread::io_port_base/count)를 TSS IOPB에
// 반영한다. kernel/core/sched는 TSS/IOPB의 존재를 몰라도 된다
// (ADR-002 HAL 경계, arch_context_switch와 같은 관례) — 실제 diff
// 기반 재프로그래밍은 arch 계층(x86_64: tss.cpp)이 담당한다.
extern "C" void arch_sync_io_permission(const kern::object::thread& next);

// M21(ADR-177, tss.hpp::sync_exception_stack) — next가 g_current가 될
// 때마다 TSS.RSP0를 next 전용 커널 스택으로 맞춘다. arch_sync_io_permission
// 과 같은 4곳(start/yield/block/exit)에서 함께 부른다.
extern "C" void arch_sync_exception_stack(const kern::object::thread& next);

namespace kern::sched {

namespace {

run_queue* g_run_queues = nullptr;
uint32_t g_node_count = 0;
kern::object::thread* g_current = nullptr;

// start() 호출 시점의 kernel_main 실행 흐름을 "버리는" 곳 — 다시 읽지
// 않는다. arch_context_switch는 첫 인자로 반드시 유효한 쓰기 위치를
// 요구하므로 자리만 마련해 둔다.
uint64_t g_bootstrap_discard_rsp = 0;

// M8 — next가 유저 스레드(owner_space가 있음)면 그 주소공간의 PML4로
// CR3를 전환해야 한다. 커널 스레드는 owner_space == nullptr이라 항상
// 0(= CR3 유지)을 반환한다 — arch_context_switch가 0을 "건드리지 않음"
// 신호로 해석한다(context_switch.S).
uint64_t next_pml4_phys(const kern::object::thread& next) {
    return next.owner_space != nullptr ? next.owner_space->page_table_root : 0;
}

// M12(ADR-141) — syscall_entry.S가 쓰는 전역 스크래치를, 지금 스위치해
// 들어가려는 스레드 전용 값으로 맞춰 둔다. 커널 스레드는 애초에 SYSCALL로
// 들어올 일이 없으니 건드리지 않는다(next_pml4_phys의 "커널 스레드는
// 0" 패턴과 같은 정신).
extern "C" uint64_t g_syscall_kernel_rsp;

void sync_syscall_kernel_rsp(const kern::object::thread& next) {
    if (next.owner_space != nullptr) {
        g_syscall_kernel_rsp = next.syscall_kernel_rsp;
    }
}

// M21(ADR-176) — scheduler.md §4의 multiplier(boost_level). "선형
// 매핑으로 시작"(§4 상단 주석)을 그대로 따른다 — boost_level=0이면
// 배율 1(기본), 승격될수록(값이 커질수록) 배율도 커진다. §5(승격
// syscall)가 아직 없어(sys_thread_boost 미구현) 지금은 모든 스레드가
// boost_level=0로 태어난 그대로 고정이라 이 배율은 사실상 항상 1이다
// — 그래도 §4가 요구하는 형태는 지금부터 실제로 소비한다(다음에
// 승격 syscall이 생기면 별도 배선 없이 바로 작동한다).
uint64_t slice_multiplier(uint32_t boost_level) { return static_cast<uint64_t>(boost_level) + 1; }

// M21(ADR-176) — LAPIC 타이머를 실제 마이크로초 단위로 보정하지
// 않았다(PIT/HPET 기반 보정은 이 마일스톤 범위 밖, YAGNI — lapic.cpp의
// busy_delay()가 AP 기동 지연을 보정 없이 흉내내는 것과 같은 정신).
// 그래서 base_time_slice_us(단위는 필드 이름 그대로 "마이크로초"이지만
// 실제로는)의 값을 그대로 "타이머 틱 수"로 소비한다 — 실제 보정이
// 필요해지면(PIT/HPET로 LAPIC 타이머 주파수를 재보고) 이 함수 하나만
// 고치면 된다.
uint64_t ticks_for(const kern::object::thread_sched_fields& sched) {
    uint64_t budget = sched.base_time_slice_us * slice_multiplier(sched.boost_level);
    return budget == 0 ? 1 : budget;  // 0이면 매 틱마다 선점(최소 보장, 굶지 않음).
}

// M21(ADR-176) — t가 (다시) g_current가 될 때마다 호출해 새 슬라이스
// 예산을 채운다. start()/yield()/block()/exit() 전부가 g_current를
// 바꾸는 모든 지점에서 이걸 불러야 한다 — 그래야 on_timer_tick()이
// "이번 슬라이스에서 남은 틱"을 정확히 추적한다.
void reset_preempt_budget(kern::object::thread& t) { t.preempt_ticks_remaining = ticks_for(t.sched); }

// M21(ADR-176) — 새로 만든 스레드의 기본 타임슬라이스(틱 수, 위
// ticks_for() 주석 참고). 특별한 근거로 고른 값은 아니다 — 고전적인
// 라운드로빈 스케줄러의 "적당한 퀀텀" 감각을 재현하는 잠정치일 뿐이라
// (관찰 기반으로 조정 가능, docs/design/open-items.md 참고),
// scheduler.md §4가 요구하는 "실제로 소비"만 충족하면 된다.
constexpr uint64_t k_default_time_slice_ticks = 20;

// kernel_band이 true면 kernel_band에서만, false면 user_band에서만 꺼낸다
// — pick_next_with_stealing()이 "커널 밴드는 노드 경계를 넘어서도
// 유저 밴드보다 항상 우선"(ADR-014)이라는 전역 순서를 만들 때 두
// 밴드를 따로 조회할 수 있어야 하기 때문에 분리했다(기존
// pick_next_locked()처럼 한 노드 안에서 두 밴드를 한 번에 보는
// 버전은 "내 노드의 유저 밴드가 다른 노드의 커널 밴드보다 먼저
// 뽑히는" 경우가 생겨 ADR-014를 어길 수 있다 — M11에서 실제로
// preferred_node=1 데모 스레드가 이 이유로 전혀 스케줄되지 않는
// 문제를 발견해 이렇게 고쳤다).
kern::object::thread* try_pick_band(run_queue& rq, bool kernel_band) {
    scoped_lock<irq_safe<spinlock>> guard(rq.lock);
    if (kernel_band) {
        if (rq.kernel_band.empty()) {
            return nullptr;
        }
        kern::object::thread& t = rq.kernel_band.front();
        decltype(rq.kernel_band)::erase(t);
        return &t;
    }
    if (rq.user_band.empty()) {
        return nullptr;
    }
    kern::object::thread& t = rq.user_band.front();
    decltype(rq.user_band)::erase(t);
    return &t;
}

// ADR-053(kernel-scheduler.md) — 이 코어의 노드가 비면 다른 노드의
// run_queue에서 훔쳐온다. ADR-053 §영향은 "임계치·탐색 순서는 구현
// 시 정한다"고 명시적으로 미뤄뒀다 — 이 협조적 스케줄러에는 타이머가
// 없어(M10 계획 §범위 밖) "일정 시간 유휴"를 측정할 수단 자체가
// 없으므로, 임계치는 사실상 0(자기 노드가 비면 즉시 훔쳐온다)이고
// 탐색 순서는 노드 번호 순 라운드로빈으로 단순화한다(ADR-053이 이미
// 이 정도의 구현 자유를 명시적으로 허용했다). 훔쳐온 스레드의
// preferred_node는 그대로 둔다(§영향 — 다음 기회에 자기 노드로 돌아갈
// 수 있게).
//
// 순서: (1) 내 노드의 커널 밴드 (2) 다른 노드의 커널 밴드(스틸) —
// ADR-014("커널 밴드는 항상 유저 밴드보다 우선")를 노드 경계에도
// 그대로 적용한다. 그다음에야 (3) 내 노드의 유저 밴드 (4) 다른
// 노드의 유저 밴드(스틸).
kern::object::thread* pick_next_with_stealing() {
    uint32_t my_node = arch_current_node_id() % g_node_count;

    for (uint32_t attempt = 0; attempt < g_node_count; ++attempt) {
        uint32_t node = (my_node + attempt) % g_node_count;
        if (kern::object::thread* t = try_pick_band(g_run_queues[node], /*kernel_band=*/true)) {
            return t;
        }
    }
    for (uint32_t attempt = 0; attempt < g_node_count; ++attempt) {
        uint32_t node = (my_node + attempt) % g_node_count;
        if (kern::object::thread* t = try_pick_band(g_run_queues[node], /*kernel_band=*/false)) {
            return t;
        }
    }
    return nullptr;
}

// M22(general-purpose-completion.md §M22, ADR-178) — pick_next_with_stealing()
// 이 고른 스레드가 kill_requested라면 그 스레드를 실제로 스케줄하지
// 않고 그 자리에서 폐기한 뒤 다시 고른다. run_queue에서는 이미
// try_pick_band()가 erase까지 마쳐 뒀으므로(제거된 상태) 여기서
// 추가로 큐를 건드릴 필요가 없다 — kern::sched::exit()의 자기 종료와
// 똑같이 "다시 enqueue하지 않는다"가 곧 폐기다. FPU 소유권 기록만
// exit()와 동일하게 정리한다(다른 코어가 이 스레드를 계속 FPU
// 소유자로 오인하지 않도록, M11b ADR-133 §결정3과 같은 이유) —
// 나머지 리소스(주소공간/핸들 테이블/스택)는 exit()과 마찬가지로
// 회수하지 않는다(기존에도 있던 누수, 이 변경이 새로 만든 것은
// 아니다).
kern::object::thread* pick_next_alive() {
    for (;;) {
        kern::object::thread* candidate = pick_next_with_stealing();
        if (candidate == nullptr) {
            return nullptr;
        }
        if (!candidate->kill_requested) {
            return candidate;
        }
        kern::klog::printf("[sched] thread killed (discarded before scheduling)\n");
        arch_fpu_thread_exiting(candidate);
    }
}

}  // namespace

void init() {
    g_node_count = kern::mm::node_count();
    if (g_node_count > kern::mm::k_max_numa_nodes) {
        g_node_count = kern::mm::k_max_numa_nodes;
    }
    if (g_node_count == 0) {
        g_node_count = 1;
    }

    // run_queue 배열도 handle_table(kernel-bootstrap-m4.md)과 같은 이유로
    // 전역에 두지 않고 여기서 mm 위에 만든다.
    auto page = kern::mm::alloc_pages(0, 0);
    if (!page.is_ok()) {
        LIBK_PANIC("kern::sched::init: no memory for run_queue array");
    }
    g_run_queues = static_cast<run_queue*>(kern::mm::phys_to_virt(page.value()));
    for (uint32_t i = 0; i < g_node_count; ++i) {
        new (&g_run_queues[i]) run_queue();
    }
}

// M9(ADR-127 §결정3) — kern::object::thread::fpu_save_area는 이미 0으로
// value-initialize돼 있다(kernel_objects.hpp의 `= {}`) — 여기서는
// FCW/MXCSR만 프로세서 리셋 기본값으로 patch한다. 이 값이 아니면
// 새 스레드가 처음 FXRSTOR/XRSTOR될 때 예외를 마스킹하지 않은 채(FCW
// 전부 0) x87 연산 중 스퓨리어스 예외를 낼 수 있다(Intel SDM Vol.1
// §13.6). legacy 영역(FCW@0, MXCSR@24)의 오프셋은 FXSAVE와 XSAVE가
// 동일하므로(M11b, ADR-133) 이 함수는 XSAVE 사용 여부와 무관하게
// 그대로 유효하다.
constexpr uint16_t k_fpu_default_fcw = 0x037F;
constexpr uint32_t k_fpu_default_mxcsr = 0x1F80;

// ADR-138 — fpu_save_area는 슬랩이 아니라 별도 order-0 페이지에서
// 나온다(slab_alloc이 64바이트 정렬을 보장하지 않아 XSAVE가 #GP를
// 낸다, kernel_objects.hpp 상단 주석 참고). 실패하면 false — 호출자가
// 이미 만든 thread/스택을 되돌려야 한다.
bool alloc_fpu_save_area(kern::object::thread* t, uint32_t node) {
    auto page = kern::mm::alloc_pages(0, node);
    if (!page.is_ok()) {
        return false;
    }
    t->fpu_save_area = static_cast<uint8_t*>(kern::mm::phys_to_virt(page.value()));
    __builtin_memset(t->fpu_save_area, 0, kern::mm::k_page_size);

    auto* fcw = reinterpret_cast<uint16_t*>(&t->fpu_save_area[0]);
    auto* mxcsr = reinterpret_cast<uint32_t*>(&t->fpu_save_area[24]);
    *fcw = k_fpu_default_fcw;
    *mxcsr = k_fpu_default_mxcsr;
    return true;
}

kern::object::thread* create_kernel_thread(void (*entry)(), kern::object::priority_band band,
                                      uint32_t preferred_node) {
    void* mem = kern::mm::slab_alloc(sizeof(kern::object::thread));
    if (mem == nullptr) {
        return nullptr;
    }
    auto* t = new (mem) kern::object::thread();
    t->sched.band = band;
    t->sched.preferred_node = preferred_node;
    t->sched.base_time_slice_us = k_default_time_slice_ticks;

    // enqueue()는 이미 preferred_node를 g_node_count로 감싼다(존재하지
    // 않는 노드를 요청해도 항상 유효한 큐에 들어가도록) — 여기서도
    // 같은 방식으로 감싸야 한다. 안 그러면 kern::mm::alloc_pages가
    // preferred_node를 그대로 "유효한 노드 인덱스"로 요구해(범위
    // 밖이면 invalid_node로 실패) 정확히 같은 preferred_node 값인데도
    // enqueue()는 받아주고 create_kernel_thread()는 거부하는
    // 불일치가 생긴다 — M11에서 실제로 이 불일치 때문에 노드 1개뿐인
    // 환경에서 preferred_node=1로 스레드 생성이 조용히 실패하는 것을
    // 발견해 고쳤다.
    uint32_t alloc_node = preferred_node % g_node_count;

    if (!alloc_fpu_save_area(t, alloc_node)) {
        kern::mm::slab_free(t, sizeof(kern::object::thread));
        return nullptr;
    }

    constexpr uint32_t k_stack_order = 2;  // 16KiB (scheduler.hpp 상단 주석)
    auto stack_page = kern::mm::alloc_pages(k_stack_order, alloc_node);
    if (!stack_page.is_ok()) {
        kern::mm::free_pages(kern::mm::virt_to_phys(t->fpu_save_area), 0);
        kern::mm::slab_free(t, sizeof(kern::object::thread));
        return nullptr;
    }

    auto* stack_base = static_cast<uint8_t*>(kern::mm::phys_to_virt(stack_page.value()));
    uint8_t* stack_top = stack_base + (static_cast<uint64_t>(kern::mm::k_page_size) << k_stack_order);

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

kern::object::thread* create_user_thread(uint64_t entry_rip, uint64_t user_rsp, uint64_t arg0,
                                    kern::object::address_space* space, kern::object::handle_table* handles) {
    void* mem = kern::mm::slab_alloc(sizeof(kern::object::thread));
    if (mem == nullptr) {
        return nullptr;
    }
    auto* t = new (mem) kern::object::thread();
    t->sched.band = kern::object::priority_band::user;
    t->sched.preferred_node = 0;
    t->sched.base_time_slice_us = k_default_time_slice_ticks;
    if (!alloc_fpu_save_area(t, 0)) {
        kern::mm::slab_free(t, sizeof(kern::object::thread));
        return nullptr;
    }
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
    auto stack_page = kern::mm::alloc_pages(k_kstack_order, 0);
    if (!stack_page.is_ok()) {
        kern::mm::free_pages(kern::mm::virt_to_phys(t->fpu_save_area), 0);
        kern::mm::slab_free(t, sizeof(kern::object::thread));
        return nullptr;
    }

    auto* stack_base = static_cast<uint8_t*>(kern::mm::phys_to_virt(stack_page.value()));
    uint8_t* stack_top = stack_base + (static_cast<uint64_t>(kern::mm::k_page_size) << k_kstack_order);

    // M12(ADR-141) — 이 스레드 전용 syscall 커널 스택 top을 미리
    // 계산해 둔다(thread::syscall_kernel_rsp 주석 참고). stack_top은
    // 아직 아무도 쓰지 않은 순수한 값이다 — 아래 손짜기 초기 컨텍스트가
    // stack_top보다 낮은 주소만 쓰고, arch_context_switch의 최초 ret가
    // 그 컨텍스트를 전부 소비(pop)하고 나면 arch_user_thread_trampoline
    // 진입 시점의 RSP가 정확히 stack_top이 된다.
    t->syscall_kernel_rsp = reinterpret_cast<uint64_t>(stack_top);

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

kern::object::thread* create_forked_thread(uint64_t saved_user_rip, uint64_t saved_user_rflags,
                                      uint64_t saved_user_rsp, uint64_t saved_rbx,
                                      uint64_t saved_rbp, uint64_t saved_r12, uint64_t saved_r13,
                                      uint64_t saved_r14, uint64_t saved_r15,
                                      kern::object::address_space* space, kern::object::handle_table* handles) {
    void* mem = kern::mm::slab_alloc(sizeof(kern::object::thread));
    if (mem == nullptr) {
        return nullptr;
    }
    auto* t = new (mem) kern::object::thread();
    t->sched.band = kern::object::priority_band::user;
    t->sched.preferred_node = 0;
    t->sched.base_time_slice_us = k_default_time_slice_ticks;
    if (!alloc_fpu_save_area(t, 0)) {
        kern::mm::slab_free(t, sizeof(kern::object::thread));
        return nullptr;
    }
    t->owner_space = space;
    t->handles = handles;

    // M12(ADR-141) — 이 스레드도 자기 전용 syscall 커널 스택이
    // 필요하다(create_user_thread와 같은 이유). 자식이 나중에 다시
    // syscall을 걸 때 이 값이 g_syscall_kernel_rsp로 동기화된다.
    constexpr uint32_t k_kstack_order = 2;
    auto stack_page = kern::mm::alloc_pages(k_kstack_order, 0);
    if (!stack_page.is_ok()) {
        kern::mm::free_pages(kern::mm::virt_to_phys(t->fpu_save_area), 0);
        kern::mm::slab_free(t, sizeof(kern::object::thread));
        return nullptr;
    }
    auto* stack_base = static_cast<uint8_t*>(kern::mm::phys_to_virt(stack_page.value()));
    uint8_t* stack_top = stack_base + (static_cast<uint64_t>(kern::mm::k_page_size) << k_kstack_order);
    t->syscall_kernel_rsp = reinterpret_cast<uint64_t>(stack_top);

    // 손짜기 초기 스택 — arch_fork_child_resume 진입 시 RSP가 정확히
    // [saved_user_rflags, saved_user_rip, saved_user_rsp] 3워드 블록의
    // 시작을 가리키게 만든다(syscall_entry.S 에필로그와 동일한 pop
    // 순서: r11,rcx,r9 순 — push는 그 반대 순서로 한다). 그 위(스택
    // 방향으로는 아래) 6워드는 arch_context_switch가 기대하는
    // 콜리세이브 레지스터 순서(rbp,rbx,r12,r13,r14,r15 — create_user_thread
    // 와 동일한 계약)인데, 0이 아니라 **부모가 SYSCALL을 실행한 순간의
    // 실제 값**을 채운다 — 그래야 자식이 그 레지스터들을 쓰는 코드를
    // 만나도(예: 호출자가 지역변수를 콜리세이브 레지스터에 두고 있었던
    // 경우) 부모와 동일하게 동작한다(진짜 POSIX fork() 의미론).
    uint64_t* sp = reinterpret_cast<uint64_t*>(stack_top);
    *(--sp) = saved_user_rsp;
    *(--sp) = saved_user_rip;
    *(--sp) = saved_user_rflags;
    *(--sp) = reinterpret_cast<uint64_t>(&arch_fork_child_resume);
    *(--sp) = saved_rbp;
    *(--sp) = saved_rbx;
    *(--sp) = saved_r12;
    *(--sp) = saved_r13;
    *(--sp) = saved_r14;
    *(--sp) = saved_r15;
    t->context_rsp = reinterpret_cast<uint64_t>(sp);

    return t;
}

void enqueue(kern::object::thread& t) {
    run_queue& rq = g_run_queues[t.sched.preferred_node % g_node_count];
    scoped_lock<irq_safe<spinlock>> guard(rq.lock);
    if (t.sched.band == kern::object::priority_band::kernel) {
        rq.kernel_band.push_back(t);
    } else {
        rq.user_band.push_back(t);
    }
}

void start() {
    // M1~M8은 노드 1개(ADR-035) — 그때는 항상 노드 0이었다. M11부터는
    // 실제로 여러 노드가 있을 수 있어 pick_next_with_stealing()이
    // "이 코어의 노드"를 먼저 보고, 비어 있으면 다른 노드를 훔쳐본다
    // (ADR-053).
    kern::object::thread* next = pick_next_alive();
    if (next == nullptr) {
        LIBK_PANIC("kern::sched::start: no runnable thread");
    }

    g_current = next;
    reset_preempt_budget(*next);
    sync_syscall_kernel_rsp(*next);
    arch_sync_io_permission(*next);
    arch_sync_exception_stack(*next);
    arch_context_switch(&g_bootstrap_discard_rsp, next->context_rsp, next_pml4_phys(*next));
    __builtin_unreachable();
}

void yield() {
    kern::object::thread* prev = g_current;

    // prev를 먼저 다시 enqueue한 뒤에 고른다(이전에는 순서가
    // 반대였다) — 그래야 "커널 밴드에 나 말고 아무도 없다"는 상황에서
    // pick_next_with_stealing()이 나 자신을 정당한 후보로 본다. 순서가
    // 뒤바뀌어 있으면(먼저 고르고 나중에 enqueue), 내가 유일하게 남은
    // 커널 밴드 스레드인 채로 yield()하는 순간 커널 밴드가 "일시적으로"
    // 비어 보여, ADR-014를 어기고 다른 노드/유저 밴드의 스레드가
    // 나보다 먼저 뽑혀 버린다 — 그 스레드가 다시 yield/block/exit하지
    // 않는 유저 스레드(예: initrun)라면 나는 영원히 다시 스케줄되지
    // 않는다. M11b에서 8라운드짜리 FPU 데모 스레드(thread_fpu_c_entry)
    // 하나만 오래 살아남는 상황을 만들고서야 이 버그가 실제로
    // 재현됐다 — M1~M11까지는 항상 "함께 도는" 커널 밴드 스레드가
    // 2개 이상이라 이 경합이 드러날 기회가 없었다.
    enqueue(*prev);
    kern::object::thread* next = pick_next_alive();
    if (next == nullptr) {
        // M22(ADR-178) — pick_next_alive()가 prev 자신을(방금 위
        // enqueue()로 다시 큐에 들어갔다가) kill_requested라서 폐기했고,
        // 그 외에는 아무도 runnable하지 않은 경우다. prev는 이미
        // 폐기됐으므로(arch_fpu_thread_exiting까지 끝남) 이 함수는 그
        // 실행 흐름으로 "돌아갈" 수 없다 — exit()의 "아무도 안 남음"
        // 경로와 동일하게 이 코어를 멈춘다.
        arch_idle_halt();
    }
    if (next == prev) {
        // 나 말고는 아무도 실행 가능하지 않다 — 방금 넣은 나 자신을
        // pick_next_with_stealing() 내부의 try_pick_band()가 그대로
        // 다시 뽑아 큐에서 제거해 줬다(erase까지 이미 끝났다 — 지금
        // "현재 실행 중" 상태로 돌아가는 것뿐이라 다시 큐에 넣을
        // 필요가 없다). 실제로 스위치할 필요도 없다 — 스위치해도
        // 결과는 같지만 arch_context_switch가 매번 CR0.TS를 다시
        // 세워 불필요한 `#NM`을 유발한다(M11b, ADR-133).
        //
        // M21(ADR-176) — 그래도 예산은 다시 채운다: on_timer_tick()이
        // 이 스레드 하나만 남았을 때도 매 틱 yield()를 부르는데, 예산을
        // 안 채우면 이 no-op 경로를 매 틱마다 타서 pick_next_with_stealing()
        // 을 불필요하게 반복 스캔한다(정확성 문제는 아니지만 낭비).
        reset_preempt_budget(*prev);
        return;
    }

    g_current = next;
    reset_preempt_budget(*next);
    sync_syscall_kernel_rsp(*next);
    arch_sync_io_permission(*next);
    arch_sync_exception_stack(*next);
    arch_context_switch(&prev->context_rsp, next->context_rsp, next_pml4_phys(*next));
    // arch_context_switch에서 돌아왔다는 것은 prev가 다시 스케줄되어
    // 이 지점부터 재개됐다는 뜻이다.
}

void block() {
    kern::object::thread* prev = g_current;

    kern::object::thread* next = pick_next_alive();
    if (next == nullptr) {
        LIBK_PANIC("kern::sched::block: no runnable thread (deadlock)");
    }

    // yield()와 달리 prev를 다시 enqueue하지 않는다 — 호출자(예: M6의
    // sys_call/sys_recv)가 prev를 이미 다른 대기열(endpoint의
    // waiting_callers/waiting_servers)에 넣어 뒀거나, 나중에 명시적으로
    // kern::sched::enqueue()할 책임을 진다.
    g_current = next;
    reset_preempt_budget(*next);
    sync_syscall_kernel_rsp(*next);
    arch_sync_io_permission(*next);
    arch_sync_exception_stack(*next);
    arch_context_switch(&prev->context_rsp, next->context_rsp, next_pml4_phys(*next));
}

[[noreturn]] void exit() {
    // prev(이전 g_current) 자체의 context_rsp는 이 함수 안에서 다시
    // 쓸 일이 없다 — block()과 달리 아무도 저장/복원하지 않는다
    // (scheduler.hpp exit() 주석: 다시 스케줄되지 않음이 핵심 보장).
    // 다만 포인터 값 자체는 arch_fpu_thread_exiting()에 넘겨야 한다
    // (M11b, ADR-133 §결정3) — 이 스레드를 아직 "FPU 소유자"로 기억하고
    // 있는 코어가 있다면 끊어진 스레드를 계속 가리키지 않도록 지운다.
    kern::object::thread* prev = g_current;
    arch_fpu_thread_exiting(prev);

    kern::object::thread* next = pick_next_alive();
    if (next == nullptr) {
        // 이 코어에서 더 이상 아무도 runnable하지 않다 — 진짜로 멈춘다.
        // prev의 context_rsp는 이제 아무도 다시 읽지 않는다(scheduler.hpp
        // exit() 주석 — 다시 스케줄되지 않음이 이 함수의 핵심 보장이다).
        arch_idle_halt();
    }

    g_current = next;
    reset_preempt_budget(*next);
    // prev를 다시 enqueue하지 않는다 — block()과 같은 메커니즘이지만,
    // block()과 달리 그 무엇도 나중에 prev를 깨우지 않는다(어떤 대기열
    // 에도 prev를 넣어 두지 않았다) — 이 스레드는 여기서 영구히 끝난다.
    uint64_t discard_rsp;
    sync_syscall_kernel_rsp(*next);
    arch_sync_io_permission(*next);
    arch_sync_exception_stack(*next);
    arch_context_switch(&discard_rsp, next->context_rsp, next_pml4_phys(*next));
    __builtin_unreachable();
}

kern::object::thread* current() { return g_current; }

void request_kill(kern::object::thread& t) { t.kill_requested = true; }

// M21(ADR-176) — idt.cpp가 EOI를 먼저 보낸 뒤 부른다(scheduler.hpp의
// on_timer_tick() 선언 주석 참고). g_current가 nullptr일 수 있는
// 유일한 시점은 kern::sched::start()가 아직 호출되기 전인데, 그때는 IDT가
// 걸려 있어도 LAPIC 타이머 자체를 아직 켜지 않았으므로(kernel_main.cpp
// 호출 순서) 실제로는 일어나지 않는다 — 그래도 방어적으로 확인한다.
void on_timer_tick() {
    kern::object::thread* cur = g_current;
    if (cur == nullptr) {
        return;
    }
    if (cur->preempt_ticks_remaining > 0) {
        --cur->preempt_ticks_remaining;
    }
    if (cur->preempt_ticks_remaining == 0) {
        yield();
    }
}

}  // namespace kern::sched
