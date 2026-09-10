// 스케줄러 골격 (docs/spec/scheduler.md §1~3, docs/plan/kernel-bootstrap.md
// M5). 노드별 run_queue + 커널/유저 2단 밴드 + 기본 라운드로빈까지만
// 다룬다 — M1~M8 실행 환경은 노드가 1개뿐이라(ADR-035) run_queue 배열은
// 사실상 원소 1개로 동작한다.
//
// M1~M20은 **협조적(cooperative)** 전환만 구현했다 — 스레드가 yield()를
// 직접 호출해야 다음 스레드로 넘어갔다(§4 승격 비례 타임슬라이스·§6
// 도네이션은 실제로 소비되지 않았다). M21(general-purpose-completion.md
// §M21, ADR-176)이 여기 실제 선점을 추가한다 — LAPIC 타이머가 주기적으로
// k_vector_timer(idt.hpp)를 걸면 idt.cpp가 on_timer_tick()을 부른다.
// **범위**: 이 커널은 유저모드(ring3) 실행 중에만 RFLAGS.IF=1이다 —
// 커널 스레드/스레드의 syscall 처리 구간은 항상 IF=0(어디서도 sti를
// 부르지 않는다, usermode.S의 IRETQ와 syscall SYSRET만 예외)이라
// 타이머가 원천적으로 끼어들 수 없다. 그래서 이 M21 구현이 실제로
// 선점하는 대상은 **유저 스레드가 ring3에서 실행 중인 순간뿐**이다 —
// 커널 스레드(owner_space==nullptr)는 여전히 100% 협조적이다(그리고
// 그래서 안전하다 — arch_context_switch가 RFLAGS를 저장/복원하지
// 않으므로, 만약 커널 코드가 IF=1로 실행되는 경로가 생기면 이 가정을
// 다시 검토해야 한다, ADR-176 참고).
#pragma once

#include <cstdint>

#include <k/intrusive_list.hpp>
#include <k/irq_safe.hpp>
#include <k/spinlock.hpp>

#include "object/kernel_objects.hpp"

namespace kern::sched {

// M33(real-libc-syscall-layer.md §M33, ADR-184, OPEN-62 해소) — 이
// 스케줄러가 목표로 하는 "타이머 틱 하나"의 실제 시간(마이크로초).
// arch 계층(x86_64: kernel_main.cpp)이 부팅 극초반(코어별로, 첫
// 유저모드 진입 전) 실제 하드웨어 타이머(LAPIC)를 이 값에 맞춰
// 보정한다(kern::arch::x86_64::calibrate_lapic_timer) — core는 이
// 상수 하나만 알고, "어떤 하드웨어로 어떻게 보정하는지"는 전혀
// 모른다(ADR-002 HAL 경계). ticks_for()가 스레드의
// base_time_slice_us(실제 마이크로초)를 이 값으로 나눠 "몇 번의
// 틱을 기다려야 하는지"를 계산한다.
constexpr uint64_t k_timer_tick_period_us = 1000;  // 1ms/틱.

struct run_queue {
    // M21(ADR-176) — 이 락을 쥔 코드가 이제 (BSP 한정) 타이머 인터럽트
    // 핸들러에서도 호출된다(on_timer_tick() -> yield() -> enqueue()/
    // try_pick_band()). 위 헤더 주석의 "커널 코드는 항상 IF=0" 불변식이
    // 지금은 재진입 데드락을 실제로 막아 주지만, 그 불변식에만 의존하는
    // 것은 취약하다 — irq_safe로 승격해 그 가정이 깨지더라도 안전하게
    // 만든다(libk/irq_safe.hpp, ADR-076이 이미 klog.cpp에 쓴 것과 같은
    // 방어적 승격).
    irq_safe<spinlock> lock;  // 노드당 1개(ADR-033)
    intrusive_list<kern::object::thread, &kern::object::thread::run_queue_hook> kernel_band;
    intrusive_list<kern::object::thread, &kern::object::thread::run_queue_hook> user_band;
};

// kern::mm::init()이 이미 호출된 뒤 실행해야 한다 — run_queue 배열을
// kern::mm::alloc_pages 위에 만든다(전역 정적 객체의 암시적 동적 초기화를
// 피한다, ADR-118 — handle_table과 같은 이유).
void init();

// 커널 스레드를 만든다. 스택(16KiB — virtual-memory-layout.md §6가
// "정확한 크기는 M5가 정한다"고 미뤄둔 것을 이 마일스톤이 잠정
// 확정한다)을 kern::mm::alloc_pages로 확보하고, 처음 스케줄될 때 entry로
// 진입하도록 컨텍스트를 미리 구성한다. 실패하면 nullptr.
//
// 알려진 단순화: 정식 "커널 스택 슬롯 영역"(2GiB, 가드 페이지,
// virtual-memory-layout.md §2)에 매핑하지 않고 physmap(kern::mm::phys_to_virt)
// 가상주소를 그대로 스택으로 쓴다 — 모든 커널 스레드가 지금은 부팅
// 때 만든 동일한 주소공간(커널 자신의 PML4)에서 돌기 때문에 이것으로
// 충분하고, 가드 페이지(스택 오버플로 조기 감지)는 나중에 필요해지면
// 추가한다.
kern::object::thread* create_kernel_thread(void (*entry)(), kern::object::priority_band band,
                                      uint32_t preferred_node);

// M8(kernel-bootstrap.md, boot.md §4) — 유저모드로 진입할 스레드를
// 만든다. entry_rip/user_rsp/arg0은 이미 space(호출자가 kern::arch::x86_64::
// create_address_space_root + map_page 등으로 다 구성해 둔 것)의
// 유저 영역 가상주소다 — 이 함수 자신은 매핑을 전혀 하지 않는다(ADR-002
// HAL 경계: kernel/core/sched는 page_table을 모른다). 이 스레드가 처음
// 스케줄될 때 arch_user_thread_trampoline(arch 훅, create_kernel_thread의
// entry 자리를 대신함)이 실행되어 CR3를 space로 전환하고(scheduler.cpp가
// context switch 시점에 이미 해 둔다) IRETQ로 유저모드에 진입시킨다.
// 실패하면 nullptr.
kern::object::thread* create_user_thread(uint64_t entry_rip, uint64_t user_rsp, uint64_t arg0,
                                    kern::object::address_space* space, kern::object::handle_table* handles);

// M12(system-servers-bringup.md §M12, ADR-142) — sys_fork의 자식
// 스레드를 만든다. create_user_thread와 달리 "처음부터 새 진입점으로
// 시작"하지 않고 "부모가 SYSCALL을 실행한 그 순간의 유저 레지스터
// 상태 그대로 재개"해야 한다(POSIX fork()가 두 번 반환하는 것처럼
// 보이게 하는 핵심) — 그래서 인자로 그 순간의 값 9개를 그대로
// 받는다(process_ops.cpp::fork_current가 syscall_entry.S의 저장된
// 레지스터 블록에서 그대로 읽어 넘겨준다). arch_fork_child_resume
// (arch 훅, syscall_entry.S)가 create_user_thread의
// arch_user_thread_trampoline 자리를 대신한다 — 이 스레드가 처음
// 스케줄되면 그 라벨로 진입해 이 9개 값으로 곧바로 SYSRET한다(RAX는
// 그 라벨 자신이 0으로 정한다 — "나는 자식이다").
kern::object::thread* create_forked_thread(uint64_t saved_user_rip, uint64_t saved_user_rflags,
                                      uint64_t saved_user_rsp, uint64_t saved_rbx,
                                      uint64_t saved_rbp, uint64_t saved_r12, uint64_t saved_r13,
                                      uint64_t saved_r14, uint64_t saved_r15,
                                      kern::object::address_space* space, kern::object::handle_table* handles);

void enqueue(kern::object::thread& t);

// run_queue에서 스레드를 하나 뽑아 그리로 실행을 넘긴다. 이 함수
// 호출자에게는 절대 돌아오지 않는다 — 그 뒤로는 스케줄된 스레드들
// 사이의 yield()로만 제어가 옮겨간다. BSP 전용(kernel_main.cpp::
// demo_sched() 끝) — AP는 아래 start_ap()를 쓴다.
[[noreturn]] void start();

// M34(real-libc-syscall-layer.md §M34, ADR-185) — BSP가 자기 단일
// 스레드 부트스트랩(init()+create_kernel_thread() 호출들)을 전부
// 마치고 자신의 start()를 부르기 직전에 딱 한 번 호출한다. 그 전에는
// run_queue가 아직 안전하지 않아(초기화 중이거나 스레드가 하나도
// 없어) AP가 참여하면 안 된다.
void mark_multicore_ready();

// AP가 자기 하드웨어 부트스트랩(lapic_enable_this_core/calibrate_lapic_timer/
// install_syscall_entry 등, smp.cpp::ap_main())을 마친 뒤 부른다 —
// mark_multicore_ready()가 불릴 때까지 자기 LAPIC 타이머 인터럽트로
// 깨는 hlt를 반복한다(busy-spin 아님).
void wait_for_multicore_ready();

// AP 전용 진입점 — start()와 같은 역할이지만 (a) 먼저
// wait_for_multicore_ready()로 대기하고, (b) 유저 밴드에만 참여하며
// (커널 밴드 데모 스레드는 멀티코어 동시 실행을 검증한 적이 없다),
// (c) 아직 유저 스레드가 하나도 없으면(BSP가 만들기 전) PANIC이
// 아니라 인터럽트로 깰 때마다 재시도한다.
[[noreturn]] void start_ap();

// 현재 스레드를 run_queue 뒤에 다시 넣고(§3 라운드로빈) 다음 스레드로
// 전환한다. 전환할 다른 스레드가 없으면(자기 자신만 runnable) 그냥
// 반환한다.
void yield();

// 현재 스레드를 run_queue에 다시 넣지 않고 다음 스레드로 전환한다 —
// M6(kernel/core/ipc)이 sys_call/sys_recv의 블로킹 대기에 쓴다. 다른
// 누군가(보통 상대방 IPC 스레드)가 나중에 kern::sched::enqueue()로 이
// 스레드를 다시 깨워야 한다 — 그러지 않으면 영원히 멈춘다. 전환할
// 다른 runnable 스레드가 없으면 LIBK_PANIC(교착 상태 — 이 협조적
// 스케줄러에는 idle 스레드가 없다).
void block();

// 현재 스레드를 영구적으로 끝낸다 — block()과 달리 그 누구도 이 스레드를
// 다시 깨우지 않는다(어차피 아무도 참조를 들고 있지 않다). 다음 runnable
// 스레드로 전환한다(kernel_band 우선, 그다음 user_band — pick_next_locked과
// 동일한 우선순위). 아무도 남지 않았으면 이 코어를 arch_idle_halt()로
// 멈춘다.
//
// 왜 필요한가: 이전에는(M7까지) 각 데모 스레드가 "충분히 넉넉한 횟수"
// yield()한 뒤 스스로 무한 hlt 루프로 들어가는 flush_yield 관례를
// 썼다 — 그러나 이 방식은 근본적으로 취약하다: 서로 다른 스레드가
// 완료까지 필요로 하는 총 yield 횟수가 다르면(예: 사전 작업이 없는
// 스레드 vs 여러 단계를 거치는 스레드), "가장 적게 필요한" 스레드가
// 가장 먼저 자기 몫을 다 쓰고 hlt로 들어가 버릴 수 있다 — 그 순간
// 그 스레드가 "현재 실행 중"이었다면, 아직 끝나지 않은 다른 스레드가
// run_queue에 아무리 남아 있어도 그들을 깨워 줄 존재가 더 이상 없어
// 기계 전체가 멈춘다(이 협조적 스케줄러에는 타이머 인터럽트가 없어
// hlt는 영원히 되돌아오지 않는다). exit()는 "내가 다시 스케줄되지
// 않는다"를 스케줄러 자신이 보장하므로(재적재하지 않음), kernel_band가
// 스레드가 하나씩 끝날 때마다 단조롭게 줄어들어 결국 진짜로 비고,
// 그제서야 user_band(유저 스레드)가 제 차례를 받는다 — M8부터 유저
// 스레드가 처음 생기면서 이 보장이 실제로 필요해졌다.
[[noreturn]] void exit();

kern::object::thread* current();

// M22(general-purpose-completion.md §M22, ADR-178) — t에게 강제 종료
// 요청 표시를 남긴다(kern::object::thread::kill_requested 참고). t 자신도,
// 이 함수를 부른 스레드도 즉시 어떤 변화를 겪지 않는다 — t가 스스로
// yield()/시간슬라이스 소진으로 run_queue에 다시 들어갔다가 스케줄러가
// 다음에 그를 뽑으려는 순간(pick_next_alive(), scheduler.cpp) 실제
// 폐기가 일어난다. 멱등이다(이미 요청된 스레드에 다시 불러도 안전).
void request_kill(kern::object::thread& t);

// M21(general-purpose-completion.md §M21, ADR-176) — LAPIC 타이머
// ISR(idt.cpp, k_vector_timer)이 매 틱 부른다. 반드시 EOI를 먼저 보낸
// 뒤 호출해야 한다(idt.cpp 호출부 주석 참고 — 이 함수가 내부적으로
// yield()를 호출하면 이 스레드의 콜스택 자체가 다른 스레드로 넘어가,
// 이 인터럽트의 iretq는 그 스레드가 나중에 다시 여기로 돌아올 때만
// 실행되므로 그 전에 EOI가 없으면 이 코어가 그때까지 다른 인터럽트를
// 못 받는다). 현재 스레드의 preempt_ticks_remaining을 하나 깎고, 0이
// 되면 yield()로 강제 전환한다(§3 라운드로빈과 동일한 정책 — 새치기
// 없음).
void on_timer_tick();

}  // namespace kern::sched
