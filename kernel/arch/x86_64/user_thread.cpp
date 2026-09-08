// arch_user_thread_trampoline — 유저 스레드가 처음 스케줄될 때 진입하는
// 자리(docs/plan/kernel-bootstrap.md M8). kernel/core/sched/scheduler.cpp가
// arch 훅으로 extern 선언해 두고, create_user_thread가 이 함수 주소를
// 새 스레드의 초기 "복귀 주소"로 심어 둔다(create_kernel_thread가
// entry 함수 주소를 심는 것과 같은 자리).
//
// arch_context_switch(context_switch.S)가 이미 CR3를 이 스레드의
// 주소공간으로 전환해 둔 뒤 여기로 들어온다 — 그래서 이 함수 자신은
// CR3를 건드리지 않는다.
#include <cstdint>

#include <object/kernel_objects.hpp>
#include <sched/scheduler.hpp>

// syscall_entry.S의 전역 스크래치 — "현재 실행 중인 유저 스레드의 커널
// 스택 top"(syscall_entry.S 상단 주석 참고). 이 스레드가 나중에 SYSCALL로
// 다시 들어올 때 이 값이 필요하므로, 유저모드로 나가기 직전인 지금이
// 그것을 기록할 유일한 기회다.
extern "C" uint64_t g_syscall_kernel_rsp;

extern "C" [[noreturn]] void enter_usermode(uint64_t rip, uint64_t rsp, uint64_t arg0);

extern "C" [[noreturn]] void arch_user_thread_trampoline() {
    uint64_t rsp_now;
    asm volatile("mov %%rsp, %0" : "=r"(rsp_now));
    g_syscall_kernel_rsp = rsp_now;

    object::thread* self = sched::current();
    enter_usermode(self->user_entry_rip, self->user_rsp, self->user_arg0);
}
