/* CPU 예외(벡터 0-31) ISR 스텁 (idt.cpp가 IDT에 등록)
 *
 * 하드웨어가 에러 코드를 안 주는 벡터는 우리가 더미 0을 대신 채워서,
 * isr_common_stub 진입 시 스택 레이아웃이 항상 [vector][error_code]
 * [rip][cs][rflags][rsp][ss]로 똑같아지게 한다(kernel::InterruptFrame과
 * 정확히 일치해야 함 - interrupt_frame.h 참고).
 */

.intel_syntax noprefix
.code64

.macro isr_no_err vec
.global isr\vec
isr\vec:
    push 0
    push \vec
    jmp isr_common_stub
.endm

.macro isr_err vec
.global isr\vec
isr\vec:
    push \vec
    jmp isr_common_stub
.endm

.section .text

isr_no_err 0
isr_no_err 1
isr_no_err 2
isr_no_err 3
isr_no_err 4
isr_no_err 5
isr_no_err 6
isr_no_err 7
isr_err    8
isr_no_err 9
isr_err    10
isr_err    11
isr_err    12
isr_err    13
isr_err    14
isr_no_err 15
isr_no_err 16
isr_err    17
isr_no_err 18
isr_no_err 19
isr_no_err 20
isr_err    21
isr_no_err 22
isr_no_err 23
isr_no_err 24
isr_no_err 25
isr_no_err 26
isr_no_err 27
isr_no_err 28
isr_err    29
isr_err    30
isr_no_err 31

/* 하드웨어 인터럽트(LAPIC) - CPU 예외와 달리 에러 코드가 없다. */
isr_no_err 32   /* 타이머(kTimerVector, timer.h) */
isr_no_err 255  /* spurious (lapic.cpp의 SVR 설정과 일치해야 함) */

/* 33~254: IOAPIC/(추후) MSI가 임의로 라우팅할 수 있는 범용 하드웨어
 * 인터럽트 벡터 - 장치마다 손으로 스텁을 추가하는 대신 전부 미리
 * 찍어내고, idt.cpp의 벡터->콜백 테이블(Idt::registerHandler)로
 * 디스패치한다(PL-2D149D8F 4~5단계). isr_table_entry가 만드는
 * kIsrDynamicStubTable[i]는 벡터(33+i)의 스텁 주소를 담는다 - idt.cpp가
 * IDT 게이트를 채울 때 이 표를 그대로 쓴다(222개를 하나하나 extern
 * 선언하지 않기 위함).
 */
.macro isr_table_entry vec
.quad isr\vec
.endm

.altmacro
.set dyn_vec, 33
.rept (254 - 33 + 1)
isr_no_err %dyn_vec
.set dyn_vec, dyn_vec+1
.endr
.noaltmacro

.section .rodata
.global kIsrDynamicStubTable
kIsrDynamicStubTable:
.altmacro
.set dyn_vec, 33
.rept (254 - 33 + 1)
isr_table_entry %dyn_vec
.set dyn_vec, dyn_vec+1
.endr
.noaltmacro

.section .text

.extern kIsrHandler
isr_common_stub:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    /* rsp가 지금 kernel::InterruptFrame의 시작을 정확히 가리킨다 -
     * System V AMD64 첫 인자(rdi)로 그대로 넘긴다. */
    mov rdi, rsp
    call kIsrHandler

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    add rsp, 16   /* vector + error_code 버림 */
    iretq
