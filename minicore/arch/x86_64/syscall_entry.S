/* x86_64 `syscall`/`sysretq` 빠른 시스템 콜 진입 경로(PN-124C105B
 * "남은 것", QU-F75DAA77 설계자 답변 "(A) swapgs + GS_BASE 정석
 * 경로"). IA32_LSTAR가 이 심볼을 가리킨다(syscall_fastpath.cpp의
 * SyscallFastPath::initForThisCore() 참고).
 *
 * `syscall` 진입 시 하드웨어가 하는 일: RIP -> RCX, RFLAGS -> R11,
 * CS/SS를 STAR[47:32] 기준으로 커널 값으로 교체, RFLAGS &= ~SFMASK.
 * **RSP는 전혀 건드리지 않는다** - 진입 직후에도 RSP는 여전히 유저
 * (ring3) 스택이다. `int 0x80`(트랩 게이트, idt.cpp)과 달리 CPU가
 * TSS.RSP0으로 자동 전환해 주지 않으므로, 이 스텁이 직접 전환한다.
 *
 * 전환 순서: swapgs로 이 코어 전용 스크래치(SyscallPerCpuScratch,
 * syscall_fastpath.cpp)를 GS_BASE로 끌어옴 -> gs:[0]에 유저 RSP를
 * 잠깐 저장 -> gs:[8](이 코어의 현재 커널 RSP, TSS.RSP0과 항상 같은
 * 값)로 RSP를 바꿔침 -> 이제부터는 안전한 커널 스택이므로 rcx/r11을
 * 자유롭게 push/pop해 보존 -> kSyscallFastDispatch(syscall_fastpath.cpp,
 * 내부적으로 int 0x80과 완전히 같은 kernel::kDispatchSyscallVerb를
 * 호출) 호출 -> 결과(RAX)를 들고 rcx/r11 복원 -> gs:[0]에 저장해 둔
 * 유저 RSP로 되돌림 -> swapgs로 유저 GS_BASE 복귀 -> sysretq.
 *
 * self-terminate(kDispatchSyscallVerb가 절대 반환하지 않는 유일한
 * 경우 - syscall.h/idt.cpp 참고)는 이 스텁의 call 지점 자체가
 * "돌아오지 않는" 것이므로 그 뒤(pop/sysretq)가 아예 실행되지 않는다
 * - ring3로 절대 복귀하지 않는다는 계약이 int 0x80 경로와 동일하게
 * 성립한다.
 */

.intel_syntax noprefix
.code64

.section .text

.global kSyscallEntry
kSyscallEntry:
    swapgs
    mov gs:[0], rsp
    mov rsp, gs:[8]

    push rcx                /* 반환 RIP(SYSCALL이 RCX에 넣어 둠) */
    push r11                /* 반환 RFLAGS(SYSCALL이 R11에 넣어 둠) */

    /* SysV 호출 규약 인자 순서(rdi, rsi, rdx)에 맞춰 옮긴다 - 원래
     * syscall 인자(RDI=arg0, RSI=arg1)는 그대로 옮기고, verb(RAX)를
     * 첫 인자 자리로 민다. rdx는 아직 안 쓰지만(syscall.h ABI가
     * 2인자까지만 정의) 먼저 옮기는 순서를 지켜 나중에 3인자로
     * 확장해도 이 스텁을 다시 안 고쳐도 되게 해 둔다. */
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax
    call kSyscallFastDispatch

    pop r11
    pop rcx
    mov rsp, gs:[0]
    swapgs
    sysretq
