// 스케줄러 골격 (docs/spec/scheduler.md §1~3, docs/plan/kernel-bootstrap.md
// M5). 노드별 run_queue + 커널/유저 2단 밴드 + 기본 라운드로빈까지만
// 다룬다 — §4(승격 비례 타임슬라이스)·§5(승격 syscall)·§6(도네이션)은
// M6 이후로 미룬다(scheduler.md §7). M1~M8 실행 환경은 노드가 1개뿐이라
// (ADR-035) run_queue 배열은 사실상 원소 1개로 동작한다.
//
// 이 마일스톤은 **협조적(cooperative)** 전환만 구현한다 — 타이머
// 인터럽트로 강제 선점하려면 IDT/APIC가 필요한데 아직 없다(M1~M8
// 어디에도 IDT 구축이 명시적으로 없음). 스레드가 yield()를 직접
// 호출해야 다음 스레드로 넘어간다. 실제 선점형 스케줄링은 인터럽트
// 인프라가 생기는 이후 마일스톤의 몫이다.
#pragma once

#include <cstdint>

#include <libk/intrusive_list.hpp>
#include <libk/spinlock.hpp>

#include "object/kernel_objects.hpp"

namespace sched {

struct run_queue {
    spinlock lock;  // 노드당 1개(ADR-033)
    intrusive_list<object::thread, &object::thread::run_queue_hook> kernel_band;
    intrusive_list<object::thread, &object::thread::run_queue_hook> user_band;
};

// mm::init()이 이미 호출된 뒤 실행해야 한다 — run_queue 배열을
// mm::alloc_pages 위에 만든다(전역 정적 객체의 암시적 동적 초기화를
// 피한다, ADR-118 — handle_table과 같은 이유).
void init();

// 커널 스레드를 만든다. 스택(16KiB — virtual-memory-layout.md §6가
// "정확한 크기는 M5가 정한다"고 미뤄둔 것을 이 마일스톤이 잠정
// 확정한다)을 mm::alloc_pages로 확보하고, 처음 스케줄될 때 entry로
// 진입하도록 컨텍스트를 미리 구성한다. 실패하면 nullptr.
//
// 알려진 단순화: 정식 "커널 스택 슬롯 영역"(2GiB, 가드 페이지,
// virtual-memory-layout.md §2)에 매핑하지 않고 physmap(mm::phys_to_virt)
// 가상주소를 그대로 스택으로 쓴다 — 모든 커널 스레드가 지금은 부팅
// 때 만든 동일한 주소공간(커널 자신의 PML4)에서 돌기 때문에 이것으로
// 충분하고, 가드 페이지(스택 오버플로 조기 감지)는 나중에 필요해지면
// 추가한다.
object::thread* create_kernel_thread(void (*entry)(), object::priority_band band,
                                      uint32_t preferred_node);

// M8(kernel-bootstrap.md, boot.md §4) — 유저모드로 진입할 스레드를
// 만든다. entry_rip/user_rsp/arg0은 이미 space(호출자가 arch_x86_64::
// create_address_space_root + map_page 등으로 다 구성해 둔 것)의
// 유저 영역 가상주소다 — 이 함수 자신은 매핑을 전혀 하지 않는다(ADR-002
// HAL 경계: kernel/core/sched는 page_table을 모른다). 이 스레드가 처음
// 스케줄될 때 arch_user_thread_trampoline(arch 훅, create_kernel_thread의
// entry 자리를 대신함)이 실행되어 CR3를 space로 전환하고(scheduler.cpp가
// context switch 시점에 이미 해 둔다) IRETQ로 유저모드에 진입시킨다.
// 실패하면 nullptr.
object::thread* create_user_thread(uint64_t entry_rip, uint64_t user_rsp, uint64_t arg0,
                                    object::address_space* space, object::handle_table* handles);

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
object::thread* create_forked_thread(uint64_t saved_user_rip, uint64_t saved_user_rflags,
                                      uint64_t saved_user_rsp, uint64_t saved_rbx,
                                      uint64_t saved_rbp, uint64_t saved_r12, uint64_t saved_r13,
                                      uint64_t saved_r14, uint64_t saved_r15,
                                      object::address_space* space, object::handle_table* handles);

void enqueue(object::thread& t);

// run_queue에서 스레드를 하나 뽑아 그리로 실행을 넘긴다. 이 함수
// 호출자에게는 절대 돌아오지 않는다 — 그 뒤로는 스케줄된 스레드들
// 사이의 yield()로만 제어가 옮겨간다.
[[noreturn]] void start();

// 현재 스레드를 run_queue 뒤에 다시 넣고(§3 라운드로빈) 다음 스레드로
// 전환한다. 전환할 다른 스레드가 없으면(자기 자신만 runnable) 그냥
// 반환한다.
void yield();

// 현재 스레드를 run_queue에 다시 넣지 않고 다음 스레드로 전환한다 —
// M6(kernel/core/ipc)이 sys_call/sys_recv의 블로킹 대기에 쓴다. 다른
// 누군가(보통 상대방 IPC 스레드)가 나중에 sched::enqueue()로 이
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

object::thread* current();

}  // namespace sched
