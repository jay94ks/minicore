/* 스케줄러 소프트웨어 컨텍스트 스위칭(PL-2D3184BC, DC-8EA1E7F6 -
 * "컨텍스트 전환 메커니즘은 소프트웨어 방식의 스위칭을 기본으로
 * 한다") - x86 하드웨어 TSS 태스크 스위칭은 안 쓴다.
 *
 * kContextSwitch(oldRspSlot=rdi, newRsp=rsi): System V AMD64 콜리
 * 세이브 레지스터(rbx/rbp/r12-r15)와 RFLAGS를 현재 스택에 push하고
 * 그 RSP를 *oldRspSlot에 저장한 뒤, RSP를 newRsp로 바꿔 그 자리의
 * 값들을 pop한다 - 마지막 ret가 그 태스크가 스위칭 아웃됐던 지점
 * (기존에 이미 한 번이라도 디스패치된 Task, 협조적 전환 전제)으로
 * 이어간다.
 *
 * [신규, 2026-09-19, PN-414BF822] `kContextSwitchToFreshTask`(아래,
 * kTaskStartTrampoline 앞)는 이 함수의 "저장" 절반은 그대로 재사용
 * 하되, "복원" 절반을 진짜 `iretq` 기반으로 바꾼 variation - 한 번도
 * 디스패치된 적 없는 Task를 인터럽트 컨텍스트(`onTick()`)에서 첫
 * 디스패치할 때만 쓴다(설계자 답변 QU-CC3BB5AE - "인터럽트에서 작업
 * 전환을 단일 메서드로 과도하게 일반화하지 말 것").
 */

.intel_syntax noprefix
.code64

.section .text

.global kContextSwitch
kContextSwitch:
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, rsi

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq
    ret

/* [신규, 2026-09-19, PN-414BF822, 설계자 답변(QU-CC3BB5AE)] onTick()
 * (타이머 인터럽트 컨텍스트)이 한 번도 디스패치된 적 없는 Task를 첫
 * 디스패치할 때 전용으로 쓰는 컨텍스트 스위치 variation - "현재
 * 실행 흐름을 저장"하는 앞부분은 kContextSwitch와 완전히 동일하다
 * (그래서 이 Task가 나중에 다시 뽑히면 평범한 kContextSwitch로
 * 정상 재개된다 - *oldRspSlot에 남은 값이 가리키는 곳은 바로 이
 * 함수 호출 다음 줄이다). 다른 점은 복원 쪽 - popfq+ret(협조적 전환
 * 전제, next->hasEverRun==false인 Task에 대해 실측으로 확인된 크래시
 * 원인) 대신, isr_common_stub(isr.S)이 쌓는 것과 정확히 같은
 * InterruptFrame을 그 자리에서 새로 지어(kResumeForkedRing3(process.cpp)
 * 가 이미 증명한 것과 같은 "InterruptFrame을 합성해 isr_common_epilogue
 * 로 점프" 기법) 진짜 iretq 한 번으로 착지시킨다 - RFLAGS(IF=1 포함)가
 * 하드웨어의 원자적 "인터럽트로부터 복귀" 그 순간에만 반영되므로,
 * popfq 직후~목적지 코드가 안정되기 전 사이에 인터럽트가 조기
 * 재중첩될 위험 자체가 없다.
 *
 * kContextSwitchToFreshTask(oldRspSlot=rdi, newStackTop=rsi,
 *                            entryFn=rdx, entryArg=rcx):
 * entryFn/entryArg는 kTaskStartTrampoline이 그대로 기대하는 rbx/r12
 * 자리에 심는다 - RIP는 이 함수(kTaskStartTrampoline) 고정, CS/SS는
 * 커널 셀렉터(0x08/0x10 - gdt.h의 kGdtKernelCodeSelector/
 * kGdtKernelDataSelector와 반드시 일치해야 함, C++ 쪽 호출부에
 * static_assert로 고정해 둠), RSP는 newStackTop(next의 커널 스택
 * 최상단) 그대로 - 가짜 콜리세이브 프레임이 차지하던 자리를 전혀
 * 건드리지 않고 완전히 새 스택을 그대로 물려준다. 나머지 GPR
 * 슬롯(rax/rcx/rdx/rsi/rdi/rbp/r8-r11/r13-r15)은 kTaskStartTrampoline이
 * 안 쓰므로 0으로 채운다.
 */
.global kContextSwitchToFreshTask
kContextSwitchToFreshTask:
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp

    mov rsp, rsi          /* next의 새 커널 스택 최상단으로 전환 - 가짜 프레임 없이 그대로 */

    lea rax, [rip + kTaskStartTrampoline]

    push 0x10             /* ssOld = kGdtKernelDataSelector */
    push rsi              /* rspOld = newStackTop 그대로(스택 전체를 next에게 넘겨준다) */
    push 0x202            /* rflags: IF=1 - 진짜 iretq 완료 시점에만 반영됨 */
    push 0x08             /* cs = kGdtKernelCodeSelector */
    push rax              /* rip = kTaskStartTrampoline */
    push 0                /* errorCode(미사용) */
    push 0                /* vector(미사용) */
    push 0                /* r15 */
    push 0                /* r14 */
    push 0                /* r13 */
    push rcx              /* r12 = entryArg - kTaskStartTrampoline이 rdi로 옮겨 씀 */
    push 0                /* r11 */
    push 0                /* r10 */
    push 0                /* r9 */
    push 0                /* r8 */
    push 0                /* rbp */
    push 0                /* rdi */
    push 0                /* rsi */
    push 0                /* rdx */
    push 0                /* rcx */
    push rdx              /* rbx = entryFn - kTaskStartTrampoline이 call로 부름 */
    push 0                /* rax */
    jmp isr_common_epilogue

/* Task::init()이 새 태스크의 스택을 이 함수로 "돌아오는" 것처럼
 * 꾸며 둔다(kContextSwitch의 pop 순서와 정확히 대응하는 레이아웃 -
 * task.cpp 참고). 진입 시 rbx=entry 함수 포인터, r12=entry에 넘길
 * arg(둘 다 kContextSwitch의 pop rbx/pop r12로 이미 복원됨).
 *
 * entry를 부르기 전에 kSyncCr3OnTaskStart(scheduler.cpp)를 먼저
 * 호출한다(SP-83A07867 §3.2 갈래② - "Task가 태어나서 처음 실행되는
 * 지점"에서 CR3를 이 Task 자신의 주소공간으로 동기화). 이미 이 Task
 * 자신의 스택으로 넘어온 뒤라 안전하다 - rbx/r12는 System V
 * 콜리세이브라 이 호출을 거쳐도 그대로 보존된다.
 *
 * entry가 반환하면 kTaskFallingToEnd로 떨어진다(예전 이름
 * kTaskStartTrampoline_halt - PL-2D3184BC "Task 종료 프로토콜",
 * QU-26F9420E 설계자 답변, 2026-09-14로 개명+확장) - C++ 쪽
 * kTaskOnFallingToEnd(scheduler.cpp)를 호출해 실제 종료 처리를
 * 맡긴다: User-Level로 격하된 Task면 자기종료 syscall만 제출하고
 * (wait 없이) 그대로 반환해 아래 hlt 루프로 들어가고, Kernel-Level
 * Task면 Scheduler::retireCurrentTask()가 이 지점으로 다시는
 * 돌아오지 않으므로(다음 Task/idle로 영구 전환) 아래 hlt 루프는
 * 그 경우엔 아예 도달하지 않는다.
 */
.global kTaskStartTrampoline
kTaskStartTrampoline:
    call kSyncCr3OnTaskStart
    mov rdi, r12
    call rbx

kTaskFallingToEnd:
    call kTaskOnFallingToEnd

kTaskFallingToEndHalt:
    hlt
    jmp kTaskFallingToEndHalt

/* [신규, 2026-09-17, PN-2008220B] Scheduler::enterIdleLoop()이 이
 * 코어 전용 idle 스택으로 "돌아오는 것처럼" 꾸며 둔 프레임을 이
 * 트램폴린으로 받는다 - kTaskStartTrampoline과 똑같은 위장이지만,
 * CR3 동기화(kSyncCr3OnTaskStart)를 부르지 않는다는 점만 다르다 -
 * 이 스택으로 처음 들어오는 시점(부팅 극초반)엔 CR3가 이미
 * Paging::init()이 만든 커널 전용 PML4 그대로라 동기화할 게 없다
 * (scheduler.cpp의 enterIdleLoop() 문서 주석 참고). rbx=
 * &Scheduler::runLoop(인자 없음, r12는 그냥 0으로 채워 안 씀).
 * runLoop()은 [[noreturn]]이라 정상적으로는 이 아래로 절대 안
 * 떨어진다 - kTaskFallingToEndHalt와 같은 이유로 방어적 hlt 루프만
 * 남겨 둔다.
 */
.global kIdleLoopTrampoline
kIdleLoopTrampoline:
    call rbx

kIdleLoopTrampolineHalt:
    hlt
    jmp kIdleLoopTrampolineHalt
