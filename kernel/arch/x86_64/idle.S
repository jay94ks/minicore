/*
 * arch_idle_halt() — 이 코어에서 더 이상 아무도 runnable하지 않을 때
 * kernel/core/sched/scheduler.cpp의 exit()가 부르는 마지막 자리
 * (docs/plan/kernel-bootstrap.md M8). HLT는 인터럽트가 오기 전까지
 * 되돌아오지 않는다 — M1~M8은 타이머/IDT가 없어(scheduler.cpp 상단
 * 주석) 이 함수는 사실상 "여기서 끝"이다. 그래도 이론상 NMI 등으로
 * 깨어날 가능성에 대비해 루프로 감싼다(관례적 안전장치).
 */
.section .text
.global arch_idle_halt
.type arch_idle_halt, @function
arch_idle_halt:
1:
    hlt
    jmp 1b
