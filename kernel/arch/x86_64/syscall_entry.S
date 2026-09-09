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
 *
 * M12(ADR-142, sys_fork)부터는 rbx/rbp/r12~r15(콜리세이브 6개)도 함께
 * push/pop한다 — SYSCALL 자체는 이 레지스터들을 전혀 건드리지 않고,
 * syscall_dispatch(C++ 함수 호출)도 SysV 호출 규약상 이들을 그대로
 * 보존하므로 **일반 syscall 경로에서는 push한 값을 그대로 다시
 * pop할 뿐이라 동작에 아무 변화가 없다** — 오직 fork_child_resume
 * (아래)만을 위한 준비다: fork()의 자식은 이 9개 값(rsp/rip/rflags +
 * 콜리세이브 6개)이 **부모가 SYSCALL을 실행한 그 순간의 실제 유저
 * 레지스터 값 그대로** 채워진 손짜기 스택으로 시작해야 진짜 POSIX
 * fork()처럼 "같은 자리에서 다시 실행을 시작"할 수 있다(process_ops.cpp::
 * fork_current가 이 스택에 있는 9워드를 그대로 읽어 자식의 초기
 * 컨텍스트에 복사한다 — %r8에 그 시작 주소를 실어 syscall_dispatch에
 * 넘긴다).
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
    pushq %rcx    /* SYSCALL이 넣어 준 유저 복귀 RIP */
    pushq %r11    /* SYSCALL이 넣어 준 유저 RFLAGS */
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %r8    /* saved-regs 배열 시작 주소(fork 전용, 그 외 syscall은 무시).
                          push 역순이라 [0]=r15,[1]=r14,[2]=r13,[3]=r12,[4]=rbp,
                          [5]=rbx,[6]=rflags(r11),[7]=rip(rcx),[8]=user_rsp */
    movq %r10, %rcx   /* 4번째 인자를 syscall 관례(r10)에서 C 호출 관례(rcx)로 옮긴다 */

    call syscall_dispatch   /* rdi,rsi,rdx,rcx,r8 = num,a1,a2,a3,saved_regs. 반환값은 RAX. */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popq %r11
    popq %rcx
    popq %r9      /* 유저 RSP 복원(RAX는 손대지 않으므로 syscall_dispatch의 반환값이 그대로 유지됨) */
    movq %r9, %rsp
    sysretq

/*
 * fork_child_resume — sys_fork로 만들어진 자식 스레드가 맨 처음
 * 실행하는 지점. arch_context_switch(context_switch.S)가 이 스레드로
 * 처음 전환할 때 이미 자기 몫의 6개(r15,r14,r13,r12,rbx,rbp)를 pop한
 * 뒤 `ret`으로 여기 뛰어든다 — 그래서 이 지점에서 RSP는 이미
 * [rflags,rip,user_rsp] 3워드 블록만 남은 상태다(create_forked_thread
 * 가 정확히 이 순서로 쌓아 뒀다). syscall_entry의 일반 에필로그(9개
 * 전부 pop)와 혼동하지 않는다 — 6개는 arch_context_switch가 이미
 * 소비했으므로 여기서는 나머지 3개만 pop한다. RAX만 0으로 미리
 * 정해 둔다는 차이뿐(그 외엔 syscall_entry의 마지막 3-pop과 동일) —
 * "이 스레드는 fork()의 자식이다"라는 POSIX 관례.
 */
.global arch_fork_child_resume
.type arch_fork_child_resume, @function
arch_fork_child_resume:
    xorl %eax, %eax
    popq %r11
    popq %rcx
    popq %r9
    movq %r9, %rsp
    sysretq
