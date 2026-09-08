// arch_user_thread_trampoline — 유저 스레드가 처음 스케줄될 때 진입하는
// 자리(docs/plan/kernel-bootstrap.md M8). kernel/core/sched/scheduler.cpp가
// arch 훅으로 extern 선언해 두고, create_user_thread가 이 함수 주소를
// 새 스레드의 초기 "복귀 주소"로 심어 둔다(create_kernel_thread가
// entry 함수 주소를 심는 것과 같은 자리).
//
// arch_context_switch(context_switch.S)가 이미 CR3를 이 스레드의
// 주소공간으로 전환해 둔 뒤 여기로 들어온다 — 그래서 이 함수 자신은
// CR3를 건드리지 않는다.
//
// M12(ADR-141) 이전에는 이 함수가 "지금 이 순간의 RSP"를 전역
// g_syscall_kernel_rsp에 기록했다(유저모드로 나가기 직전이 유일한
// 기회였으므로). 지금은 create_user_thread가 스레드별 커널 스택
// top을 미리 계산해 thread::syscall_kernel_rsp에 넣어 두고,
// scheduler.cpp가 이 스레드로 스위치할 때마다 전역을 그 값으로 맞춰
// 두므로(sync_syscall_kernel_rsp) 이 함수는 더 이상 그 전역을 건드릴
// 필요가 없다.
#include <object/kernel_objects.hpp>
#include <sched/scheduler.hpp>

extern "C" [[noreturn]] void enter_usermode(uint64_t rip, uint64_t rsp, uint64_t arg0);

extern "C" [[noreturn]] void arch_user_thread_trampoline() {
    object::thread* self = sched::current();
    enter_usermode(self->user_entry_rip, self->user_rsp, self->user_arg0);
}
