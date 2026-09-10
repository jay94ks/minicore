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

/*
 * arch_wait_for_interrupt() — M34(real-libc-syscall-layer.md §M34).
 * arch_idle_halt()와 달리 [[noreturn]]이 아니다 — 딱 한 번 HLT하고
 * (어떤 인터럽트든 오면 그 즉시) 호출자에게 돌아온다. kern::sched::
 * start_ap()/wait_for_multicore_ready()가 "아직 실행할 유저 스레드가
 * 없다"를 표현하는 데 쓴다 — 이 코어 자신의 LAPIC 타이머(M33)가
 * 주기적으로 이 HLT를 깨워 주므로 busy-spin이 아니다.
 */
.global arch_wait_for_interrupt
.type arch_wait_for_interrupt, @function
arch_wait_for_interrupt:
    hlt
    ret
