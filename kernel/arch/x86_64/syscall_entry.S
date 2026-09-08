/*
 * syscall_entry — SYSCALL 명령으로 유저모드에서 커널로 들어오는 지점
 * (docs/plan/kernel-bootstrap.md M8). syscall.cpp의 install_syscall_entry()가
 * IA32_LSTAR MSR에 이 라벨의 주소를 적재해 둔다.
 *
 * 관례(이 커널 자체 ABI — 리눅스의 "번호는 RAX" 관례를 따르지 않는다):
 *   RDI = syscall 번호, RSI/RDX = 인자1/2, R10 = 인자3(RCX 자리는
 *   하드웨어가 SYSCALL 시점에 복귀 RIP로 이미 덮어써서 못 쓴다 — 리눅스와
 *   같은 이유로 R10을 대신 쓴다), RAX = 반환값.
 *
 * SYSCALL은 스택을 바꿔주지 않는다(RSP가 유저 스택 그대로) — 이 스텁이
 * 직접 커널 스택으로 전환한다. g_syscall_kernel_rsp는 "현재 실행 중인
 * 유저 스레드의 커널 스택 top"을 가리키는 단일 전역 스크래치 값이다
 * (user_thread.cpp의 arch_user_thread_trampoline이 그 스레드가 처음
 * 스케줄될 때 채워 둔다) — M8은 유저 스레드가 하나뿐이고 재진입도
 * 없어(단일 코어 협조적 스케줄러, 이 호출이 블록되는 동안 같은
 * 스레드가 또 syscall할 수 없다) 스레드별로 따로 관리하지 않는다.
 */
.section .bss
.balign 8
.global g_syscall_kernel_rsp
g_syscall_kernel_rsp:
    .skip 8
.global g_syscall_saved_user_rsp
g_syscall_saved_user_rsp:
    .skip 8

.section .text
.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    movq %rsp, g_syscall_saved_user_rsp(%rip)
    movq g_syscall_kernel_rsp(%rip), %rsp

    pushq %rcx    /* SYSCALL이 넣어 준 유저 복귀 RIP — SYSRET가 다시 쓸 때까지 보존 */
    pushq %r11    /* SYSCALL이 넣어 준 유저 RFLAGS — 위와 같은 이유 */

    movq %r10, %rcx   /* 4번째 인자를 syscall 관례(r10)에서 C 호출 관례(rcx)로 옮긴다 */

    call syscall_dispatch   /* rdi,rsi,rdx,rcx = num,a1,a2,a3 — SysV와 그대로 일치 */

    popq %r11
    popq %rcx
    movq g_syscall_saved_user_rsp(%rip), %rsp
    sysretq
