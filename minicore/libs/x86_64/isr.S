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
.extern kEnterInterruptDepth
.extern kLeaveInterruptDepth
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

    /* [신규, 2026-09-20, SP-5130284C] 이 코어의 인터럽트 깊이 카운터를
     * 1 올린다 - 반드시 위 15개 레지스터 push가 전부 끝난 뒤라야 한다
     * (그래야 진짜 인터럽트당한 값들이 이미 스택/메모리에 안전하게
     * 있고, 이 call이 clobber하는 스크래치 레지스터는 더 이상 "살아
     * 있는" 값이 아니다). balanced call/ret이라 rsp는 이 call 전후로
     * 정확히 같은 값으로 돌아오므로 아래 InterruptFrame 레이아웃에
     * 전혀 영향이 없다. */
    call kEnterInterruptDepth

    /* rsp가 지금 kernel::InterruptFrame의 시작을 정확히 가리킨다 -
     * System V AMD64 첫 인자(rdi)로 그대로 넘긴다. */
    mov rdi, rsp
    call kIsrHandler

    /* [신규, 2026-09-20, SP-5130284C] 이 인터럽트가 "자연스럽게"(같은
     * Task로 그대로 복귀) 끝나는 경로 - 깊이 카운터를 1 내린다.
     * kContextSwitch(FromISR가 아닌 쪽, 협조적 전환)는 인터럽트 진입
     * 자체가 없었으므로 이 카운터를 전혀 건드리지 않고, `kContextSwitchFromISR`
     * (다른 Task로 전환하며 이 인터럽트를 끝내는 경로)는 이 지점을
     * 거치지 않는 대신 context_switch.S 자기 안에서 직접 감소시킨다
     * (rsp를 새 Task의 고정 TaskTcb 블록으로 바꾸기 **직전** - 그 뒤로는
     * 이 call이 그 블록 메모리 위에 반환주소를 push해 레지스터 필드를
     * 깨뜨릴 위험이 있어 반드시 그 전에 해야 한다). 두 경로 모두
     * "인터럽트 진입 하나당 정확히 한 번의 감소"를 지킨다. */
    call kLeaveInterruptDepth

    /* [신규, PN-44C91D6E, fork() 자식 재개 경로] 순수 레이블 추가 -
     * 기존 흐름(위 call 직후 자연히 여기로 떨어짐)은 전혀 안 바뀐다.
     * kResumeForkedRing3(process.cpp)가 자기 스택 위에 InterruptFrame과
     * 정확히 같은 레이아웃(rax..r15,vector,errorCode,rip,cs,rflags,
     * rspOld,ssOld 순, 위 push 순서와 바이트 단위로 일치)을 만든 뒤
     * 이 레이블로 직접 jmp해 재사용한다 - "부모가 트랩한 바로 그
     * 지점에서 자식이 재개"하는 걸 이 검증된 pop+iretq 시퀀스 그대로
     * 구현한다. */
    .global isr_common_epilogue
isr_common_epilogue:
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
