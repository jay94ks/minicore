/* devmgr 진입점(crt0) - PN-A0F72A3A 착수 순서 3번 선행 확인("argv 실제
 * 전달 ABI") 결과: 커널(Process::execImage()/kSetupInitialUserStack(),
 * PN-E35294B8/QU-B9EB45E4)은 이미 SysV AMD64 관례대로 진입 시점의
 * 유저 스택에 argc/argv/envp(+빈 auxv)를 실제로 지어 두지만, 이
 * 프로젝트의 어떤 유저 서비스도 그 값을 실제로 꺼내는 코드가 없었다
 * (`_start()`가 인자 없이 선언돼 스택을 그냥 무시) - 이 파일이 그
 * 변환(스택 -> SysV 함수 호출 규약 레지스터)을 devmgr에 한해 처음
 * 채운다.
 *
 * 진입 시점 %rsp 레이아웃(kSetupInitialUserStack이 지은 그대로,
 * process.cpp:559-574 확인):
 *   [rsp+0]              = argc
 *   [rsp+8]               = argv[0]
 *   ...
 *   [rsp+8+8*argc]        = NULL (argv 종단)
 *   [rsp+8+8*(argc+1)]    = envp[0]
 *   ...
 *                           NULL (envp 종단)
 *                           auxv (AT_NULL 타입+값, 이 커널은 그 외
 *                           항목을 아직 안 채움)
 * 이 진입 RSP 자체가 이미 16-바이트 정렬이다(SysV 관례, 위 커널
 * 쪽 주석 "_start 진입 RSP는 16-정렬이어야 함"과 동일한 전제) - 아래
 * `and $-16, %rsp`는 그 전제가 깨지는 경우에 대비한 방어적 재정렬일
 * 뿐 정상 경로에서는 아무것도 바꾸지 않는다.
 */
.text
.globl _start
.type _start, @function
_start:
    xor %ebp, %ebp              /* 콜스택 최상단 표식(디버거 관례, backtrace 종료 지점) */
    mov (%rsp), %rdi            /* argc -> kDevmgrMain 1번째 인자(rdi) */
    lea 8(%rsp), %rsi           /* argv -> 2번째 인자(rsi) */
    lea 8(%rsi,%rdi,8), %rdx    /* envp = argv + argc + 1(NULL 종단 칸) -> 3번째 인자(rdx) */
    and $-16, %rsp              /* call 직전 16-정렬 보장(SysV 관례, 방어적) */
    call kDevmgrMain
    /* kDevmgrMain은 반환하지 않는다(essential 계약, main.cpp 참고) -
     * 혹시 반환해도 즉시 essential-death PANIC을 유발하는 대신 여기서
     * 멈춘다(디버깅 시 "왜 여기 있지"가 더 명확하도록). */
1:  hlt
    jmp 1b
.size _start, . - _start
