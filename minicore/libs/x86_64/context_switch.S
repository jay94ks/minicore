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
 * arg(둘 다 kContextSwitch의 pop rbx/pop r12로 이미 복원됨). entry가
 * 반환하면(v1은 종료 프로토콜이 없음 - 스케줄러 자체가 아직 없음)
 * 그냥 무한 hlt로 멈춘다.
 */
.global kTaskStartTrampoline
kTaskStartTrampoline:
    mov rdi, r12
    call rbx

kTaskStartTrampoline_halt:
    hlt
    jmp kTaskStartTrampoline_halt
