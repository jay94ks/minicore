/* 스케줄러 소프트웨어 컨텍스트 스위칭(PL-2D3184BC, DC-8EA1E7F6 -
 * "컨텍스트 전환 메커니즘은 소프트웨어 방식의 스위칭을 기본으로
 * 한다") - x86 하드웨어 TSS 태스크 스위칭은 안 쓴다.
 *
 * kContextSwitch(oldRspSlot=rdi, newRsp=rsi): System V AMD64 콜리
 * 세이브 레지스터(rbx/rbp/r12-r15)와 RFLAGS를 현재 스택에 push하고
 * 그 RSP를 *oldRspSlot에 저장한 뒤, RSP를 newRsp로 바꿔 그 자리의
 * 값들을 pop한다 - 마지막 ret가 그 태스크가 스위칭 아웃됐던 지점
 * (또는 kTaskStartTrampoline, 처음 실행되는 태스크의 경우)으로
 * 이어간다.
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
