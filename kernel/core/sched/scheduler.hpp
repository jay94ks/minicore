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

object::thread* current();

}  // namespace sched
