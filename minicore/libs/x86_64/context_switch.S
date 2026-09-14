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
    mov rdi, r12
    call rbx

kTaskFallingToEnd:
    call kTaskOnFallingToEnd

kTaskFallingToEndHalt:
    hlt
    jmp kTaskFallingToEndHalt
