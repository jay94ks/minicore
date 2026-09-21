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
.extern kEnterInterruptStack
.extern kLeaveInterruptStack
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

    /* [갱신, 2026-09-21, SP-A252E82F "인터럽트 컨텍스트 재설계"] 벡터
     * 번호(InterruptFrame::vector, 위 15개 push 직후 [rsp+120] - 구조체
     * 레이아웃과 정확히 일치해야 함)로 두 경로 중 하나를 고른다. 전체
     * 저장소 `sti` 전수 조사로 확인한 대로 일반(마스커블) 인터럽트는
     * 절대 서로 중첩되지 않으므로, 회피 불가능한 예외/NMI 다섯 개
     * (#DB=1, NMI=2, #DF=8, #PF=14, #MC=18 - gdt.cpp가 이 다섯 벡터의
     * TSS.ISTn을 채워 둠)만 하드웨어가 이미 전용 IST 스택으로 rsp를
     * 강제 전환해 뒀고, 나머지 전 벡터는 절대 중첩될 수 없으므로
     * 카운터 없이 매번 무조건 소프트웨어 스왑하면 충분하다. */
    mov eax, dword ptr [rsp + 120]
    cmp eax, 1
    je .Lisr_ist_vector
    cmp eax, 2
    je .Lisr_ist_vector
    cmp eax, 8
    je .Lisr_ist_vector
    cmp eax, 14
    je .Lisr_ist_vector
    cmp eax, 18
    je .Lisr_ist_vector

    /* [일반 벡터 경로] rdi = 지금 rsp(= InterruptFrame 시작 주소,
     * 아직 스왑 전) - kEnterInterruptStack의 인자(나중에 kLeaveInterruptStack이
     * 돌려줄 원래 rsp)로도, 잠시 뒤 kIsrHandler에 넘길 frame 포인터로도
     * 그대로 쓴다. call 이후 rdi는 caller-saved라 보존을 보장할 수
     * 없으므로(SysV ABI), call 직후 rsp에서 다시 계산한다 - balanced
     * call/ret이라 이 시점 rsp는 스왑 전 그대로다. 일반 벡터는 절대
     * 중첩되지 않으므로(위 주석) 조건 분기 없이 항상 스왑한다. */
    mov rdi, rsp
    call kEnterInterruptStack
    mov rdi, rsp
    mov rsp, rax
    call kIsrHandler

    /* 자연 복귀 - 원래(Task 자신의) rsp로 되돌려야 아래 pop들이 이
     * 인터럽트가 실제로 push된 그 자리에서 정확히 읽는다.
     * `kContextSwitch`(FromISR가 아닌 쪽, 협조적 전환)는 인터럽트
     * 진입 자체가 없었으므로 이 스왑을 전혀 건드리지 않고,
     * `kContextSwitchFromISR`(다른 Task로 전환하며 이 인터럽트를
     * 끝내는 경로)는 이 지점을 아예 거치지 않는다(다른 Task로 영구
     * 전환하며 이 진입을 끝내므로, "원래 스택으로 되돌릴" 필요 자체가
     * 없다 - 카운터가 없으므로 짝을 맞출 것도 없다). */
    call kLeaveInterruptStack
    mov rsp, rax
    jmp isr_common_epilogue

.Lisr_ist_vector:
    /* [IST 벡터 경로] 하드웨어가 이미 이 벡터 전용 고정 스택으로 rsp를
     * 전환해 뒀다 - 소프트웨어 스왑이 필요 없다(애초에 카운터도 없어
     * "중첩인지" 판단할 필요조차 없음 - kIsrHandler 쪽이 frame->rspOld로
     * 직접 판단한다, idt.cpp 참고). rdi = 지금 rsp가 곧 InterruptFrame
     * 시작 주소. */
    mov rdi, rsp
    call kIsrHandler
    /* isr_common_epilogue로 그대로 흘러 들어간다(스왑이 없었으므로
     * rsp를 되돌릴 필요도 없음) - 아래 fork() 재개 경로 주석 참고. */

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
