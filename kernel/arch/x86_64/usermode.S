/*
 * enter_usermode(uint64_t rip, uint64_t rsp, uint64_t arg0) — 처음으로
 * ring3(유저모드)에 진입한다(docs/plan/kernel-bootstrap.md M8,
 * docs/spec/boot.md §4). SYSRET가 아니라 IRETQ를 쓴다 — SYSRET는 직전에
 * SYSCALL이 RCX/R11에 남겨 둔 복귀 RIP/RFLAGS를 "되돌아간다"는 전제라
 * 한 번도 SYSCALL을 실행한 적 없는 새 스레드의 최초 진입에는 맞지
 * 않는다. IRETQ는 스택에 [RIP, CS, RFLAGS, RSP, SS] 5워드만 쌓아 두면
 * 특권 하강(ring0→ring3)까지 포함해 임의로 구성 가능하다 — TSS(RSP0)는
 * ring3→ring0 방향(인터럽트로 되돌아올 때)에만 필요하므로 이 방향은
 * TSS 없이도 동작한다.
 *
 * arg0은 RDI에 그대로 남겨 initrun의 진입점(boot.md §6 — "첫 인자
 * 레지스터"를 boot_info 전달에 쓴다)이 평범한 함수 호출처럼
 * `_start(const boot_info* info)`로 받을 수 있게 한다.
 *
 * DS/ES/FS/GS는 건드리지 않는다 — CPL이 0→3으로 바뀌는 순간 CPU가
 * "DPL이 새 CPL보다 높은(=0인) 세그먼트"를 자동으로 널 셀렉터로
 * 바꿔 버리므로(Intel SDM), 지금 커널 데이터 셀렉터(0x20, DPL0)가
 * 들어 있는 이 레지스터들은 IRETQ 직후 자동으로 널이 된다 — 롱 모드
 * 플랫 메모리 모델에서는 이 상태로도 일반 메모리 접근에 문제가 없다.
 */
.section .text
.global enter_usermode
.type enter_usermode, @function
enter_usermode:
    /* 인자(SysV): rdi=rip, rsi=rsp, rdx=arg0.
     * rdi를 먼저 다른 레지스터로 옮겨 두고, 마지막에 rdi=arg0으로
     * 정리한다(유저 진입 시 rdi가 arg0이어야 하므로). */
    movq %rdi, %rax        /* rax = rip(임시 보관) */
    movq %rdx, %rdi        /* rdi = arg0 — 이제부터 유저 진입까지 안 건드린다 */

    pushq $0x2B             /* SS = user_data64(0x28) | RPL3 */
    pushq %rsi               /* RSP = 유저 스택 top */
    pushq $0x202             /* RFLAGS: IF=1(bit9), bit1은 항상 1 */
    pushq $0x33               /* CS = user_code64(0x30) | RPL3 */
    pushq %rax                 /* RIP */
    iretq
