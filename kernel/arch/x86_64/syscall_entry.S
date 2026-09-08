/*
 * syscall_entry — SYSCALL 명령으로 유저모드에서 커널로 들어오는 지점
 * (docs/plan/kernel-bootstrap.md M8, M12/ADR-141로 스레드별 스택 지원
 * 추가). syscall.cpp의 install_syscall_entry()가 IA32_LSTAR MSR에 이
 * 라벨의 주소를 적재해 둔다.
 *
 * 관례(이 커널 자체 ABI — 리눅스의 "번호는 RAX" 관례를 따르지 않는다):
 *   RDI = syscall 번호, RSI/RDX = 인자1/2, R10 = 인자3(RCX 자리는
 *   하드웨어가 SYSCALL 시점에 복귀 RIP로 이미 덮어써서 못 쓴다 — 리눅스와
 *   같은 이유로 R10을 대신 쓴다), RAX = 반환값.
 *
 * SYSCALL은 스택을 바꿔주지 않는다(RSP가 유저 스택 그대로) — 이 스텁이
 * 직접 커널 스택으로 전환한다. g_syscall_kernel_rsp는 "지금 이 순간
 * 실행 중인 스레드"의 커널 스택 top을 가리킨다 — scheduler.cpp가 그
 * 스레드로 전환할 때마다(sync_syscall_kernel_rsp) 미리 맞춰 둔다.
 *
 * M8은 유저 RSP도 전역 스크래치(g_syscall_saved_user_rsp)에 저장했다 —
 * 유저 스레드가 하나뿐이고 그 스레드의 IPC Call도 도중에 다른 유저
 * 스레드로 넘어갈 일이 없어 안전했다. M12(ADR-141)부터는 그 전역을
 * 없애고 유저 RSP를 **이 스레드 자신의(이제 스레드별로 분리된)
 * 커널 스택에 직접 push**한다 — 이러면 이 스레드가 syscall 처리 중
 * (sys_call 등이 블로킹돼) 다른 스레드로 컨텍스트 스위치가 나가도,
 * 그 값은 이 스레드의 context_rsp 저장/복원 메커니즘에 자동으로
 * 실려 함께 보존된다(원래 있던 별도 전역 하나를 스레드 수만큼
 * 안전하게 늘린 것과 같은 효과 — 전역을 늘리는 대신 이미 스레드마다
 * 분리돼 있는 자원인 스택 자체를 썼을 뿐이다).
 */
.section .bss
.balign 8
.global g_syscall_kernel_rsp
g_syscall_kernel_rsp:
    .skip 8

.section .text
.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    movq %rsp, %r9                    /* 유저 RSP를 레지스터에 임시 보관 */
    movq g_syscall_kernel_rsp(%rip), %rsp

    pushq %r9     /* 유저 RSP — 이 스레드 전용 스택 위, block()을 거쳐도 함께 보존됨 */
    pushq %rcx    /* SYSCALL이 넣어 준 유저 복귀 RIP — SYSRET가 다시 쓸 때까지 보존 */
    pushq %r11    /* SYSCALL이 넣어 준 유저 RFLAGS — 위와 같은 이유 */

    movq %r10, %rcx   /* 4번째 인자를 syscall 관례(r10)에서 C 호출 관례(rcx)로 옮긴다 */

    call syscall_dispatch   /* rdi,rsi,rdx,rcx = num,a1,a2,a3 — SysV와 그대로 일치. 반환값은 RAX. */

    popq %r11
    popq %rcx
    popq %r9      /* 유저 RSP 복원(RAX는 손대지 않으므로 syscall_dispatch의 반환값이 그대로 유지됨) */
    movq %r9, %rsp
    sysretq
