/*
 * arch_context_switch(uint64_t* old_rsp_out, uint64_t new_rsp) —
 * docs/spec/scheduler.md, kernel/core/sched/scheduler.cpp가 부르는
 * 커널 전용 훅(ADR-002 HAL 경계 패턴 — arch_irq_save/restore와 동일).
 *
 * SysV x86-64 콜리세이브 레지스터(rbp, rbx, r12~r15)만 스택에 저장하면
 * 충분하다 — 나머지(caller-saved)는 호출자가 이미 자기 책임으로
 * 처리했거나 신경 쓰지 않는 값들이다. 현재 rsp를 *old_rsp_out에 적어
 * 두고, new_rsp로 스택을 바꾼 뒤 저장된 콜리세이브 레지스터를 복원하고
 * ret한다 — new_rsp가 가리키는 스택은 (a) 예전에 이 함수가 다른
 * 스레드로부터 전환하며 저장해 둔 것이거나, (b) sched::create_kernel_thread가
 * 손으로 흉내 내 만든 "처음 실행되는 스레드"용 스택이다. 두 경우 모두
 * 이 함수의 관점에서는 구분할 필요가 없다 — 그냥 저장된 순서대로
 * 되읽고 ret할 뿐이다.
 */
.section .text
.global arch_context_switch
.type arch_context_switch, @function
arch_context_switch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, (%rdi)   /* *old_rsp_out = 현재 rsp */
    mov %rsi, %rsp     /* rsp = new_rsp */

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
