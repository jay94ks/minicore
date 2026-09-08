/*
 * ISR 진입 스텁 256개 + 공용 트램폴린(isr_common) — idt.hpp/idt.cpp가
 * 채우는 IDT 게이트가 가리키는 실제 코드. 하드웨어 예외 벡터 중
 * 8/10/11/12/13/14/17/21/29/30번(Intel SDM Vol.3 §6.15)은 CPU가 이미
 * error_code를 스택에 넣어 주므로 그대로 두고, 나머지는 자리를 맞추려고
 * 가짜 0을 밀어 넣는다 — 그래야 idt.hpp::interrupt_frame이 모든 벡터에서
 * 같은 오프셋을 갖는다.
 *
 * isr_stub_table(.rodata, void* [256])은 idt.cpp가 IDT 게이트를 채울 때
 * 참조하는 유일한 자리 — 256개 심벌을 C++ 쪽에서 extern "C"로 하나하나
 * 선언할 필요가 없게 한다.
 */
.altmacro

/* vec에 해당하는 하드웨어 예외가 error_code를 이미 스택에 남기면 아무
 * 것도 안 하고, 아니면 가짜 0을 민다 — 그 다음 공통으로 vector 번호를
 * 밀고 isr_common으로 점프한다. */
.macro isr_stub_body vec
.global isr_stub_\vec
isr_stub_\vec:
    .if \vec==8
    .elseif \vec==10
    .elseif \vec==11
    .elseif \vec==12
    .elseif \vec==13
    .elseif \vec==14
    .elseif \vec==17
    .elseif \vec==21
    .elseif \vec==29
    .elseif \vec==30
    .else
        pushq $0
    .endif
    pushq $\vec
    jmp isr_common
.endm

.macro isr_table_entry vec
    .quad isr_stub_\vec
.endm

.section .text
.set gen_vec, 0
.rept 256
    isr_stub_body %gen_vec
    .set gen_vec, gen_vec+1
.endr

/* idt.hpp::interrupt_frame과 정확히 같은 순서로 GPR을 저장/복원한다 —
 * 마지막에 push한 레지스터(r15)가 %rsp가 가리키는 가장 낮은 주소에
 * 온다. */
isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, %rdi
    call interrupt_dispatch

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp   /* vector + error_code 버림 */
    iretq

.section .rodata
.balign 8
.global isr_stub_table
isr_stub_table:
.set gen_vec, 0
.rept 256
    isr_table_entry %gen_vec
    .set gen_vec, gen_vec+1
.endr
