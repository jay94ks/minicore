/* 스케줄러 소프트웨어 컨텍스트 스위칭(PL-2D3184BC, DC-8EA1E7F6 -
 * "컨텍스트 전환 메커니즘은 소프트웨어 방식의 스위칭을 기본으로
 * 한다") - x86 하드웨어 TSS 태스크 스위칭은 안 쓴다.
 *
 * [갱신, 2026-09-20, PN-81E49523 2단계, 설계자 답변(QU-2FC61718)+후속
 * 지시들] 이 파일은 호출 맥락(일반 C++ 호출 vs 인터럽트 핸들러 내부)에
 * 따라 서로 다른 두 함수로 나뉜다 - 둘 다 "복원" 쪽은 예외 없이
 * `isr_common_epilogue`(isr.S)로 점프해 진짜 `iretq`로 착지한다(케이스
 * 1-3/#DB와 완전히 동일한 메커니즘 - 케이스 구분 없이 전부 `TaskTcb`
 * =`InterruptFrame` 하나로 통일됨, PN-81E49523).
 *
 * **[갱신, 2026-09-20, 설계자 지시("TaskTcb 자체를 Task 구조체에
 * 계속 유지해두고, kContextSwitch에 바로 넘길 수 있는 형태로 유지" +
 * "커널 스택을 할당하는 방식으로 Tcb를 유지하지 말고, Slab 할당자를
 * 변형하든 어떻게 하든해서 Tcb 자체를 할당하여 활용")]** `Task::tcb`/
 * `AsyncTask::tcb`는 이제 그 Task 자신의 커널 스택 어딘가를 가리키는
 * 게 아니라, `GenericSlabAllocator`로 별도 할당된 고정 176바이트
 * `TaskTcb` 전용 블록을 가리킨다(task.cpp/async_task.cpp의 `init()`
 * 참고) - 이 블록의 수명은 그 Task 자신과 같고, 매번 새로 할당하지
 * 않고 같은 블록의 필드를 계속 덮어써 재사용한다. 그 결과:
 *
 * - `kContextSwitch`(아래)의 "저장" 절반은 더 이상 현재 스택에
 *   `push`하지 않는다 - `*oldTcbSlot`이 가리키는 그 **미리 할당된
 *   고정 블록**에 레지스터 값을 필드별로 직접 `mov`한다(주소 하나만
 *   스크래치로 쓰고 나머지 레지스터는 전부 원래 값 그대로 기록 -
 *   아래 주석 참고).
 * - `kContextSwitchFromISR`(아래)의 "저장" 절반도 마찬가지로, 하드웨어가
 *   만든 진짜 InterruptFrame(`currentFrame`, 인터럽트가 트랩한
 *   Task 자신의 커널 스택 위)의 22필드 값을 그 고정 블록으로 **복사**
 *   한다(가리키기만 하지 않음 - 그 스택 공간은 곧 재사용/무효화될 수
 *   있어 별도 소유의 고정 블록에 값을 옮겨 둬야 한다).
 * - "복원" 쪽은 두 함수 모두 동일: `newTcb`(고정 블록 자신의 주소)를
 *   그대로 새 RSP로 삼아(`mov rsp, newTcb`) `isr_common_epilogue`로
 *   점프한다 - 그 176바이트 블록 자체가 InterruptFrame과 완전히 같은
 *   모양이라 별도 복사 없이 그대로 iretq 소스로 쓸 수 있다("포인터를
 *   넘겨서 오버헤드 없이 스위칭").
 *
 * kContextSwitch(oldTcbSlot=rdi, newTcb=rsi): 평범한 C++ 함수 호출로
 * 불릴 때(Scheduler::parkCurrent()/yieldCurrent() 등, 인터럽트에
 * 중첩되지 않은 협조적 전환) 쓴다.
 *
 * kContextSwitchFromISR(oldTcbSlot=rdi, newTcb=rsi, currentFrame=rdx):
 * 인터럽트 핸들러 내부(Scheduler::onTick()/onForcedMigration())에서
 * 쓴다.
 */

.intel_syntax noprefix
.code64

.section .text

.global kContextSwitch
kContextSwitch:
    /* 진입 시 [rsp+0]=반환 주소(향후 rip) - 이 위치는 `call`이 실제로
     * 그 값을 써 둔 데이터라 `mov`로 읽어도 된다. **반환 직후 호출자가
     * 갖게 될 rsp(향후 rspOld)는 스택 어디에도 데이터로 저장돼 있지
     * 않다** - `push rax`+`call`로 두 번 내려간 지금 rsp 기준
     * `rsp+16`이 바로 그 "호출자가 반환 직후 가질 rsp" 자신이므로,
     * 그 위치를 `lea`로 계산해야 한다(`mov`로 읽으면 그 주소에 우연히
     * 있던 호출자의 다른 스택 내용을 읽어 온다 - 아래 실제 계산부의
     * 수정 이력 참고). rdi=oldTcbSlot(TaskTcb**), rsi=newTcb(TaskTcb*)
     * - 함수 내내 원래 값 그대로 필요할 때 다시 쓴다(어떤 mov/push/pop
     * 도 이 두 레지스터의 "값"을 안 바꾼다 - push는 그 값을 스택에
     * 복사만 할 뿐 레지스터 자체는 그대로).
     *
     * rax를 "쓰기 대상 주소"(*oldTcbSlot, 즉 이 Task 전용 고정
     * TaskTcb 블록의 주소) 스크래치로 쓴다 - rax의 원래 값은 맨 끝에
     * 스택(A, 지금 이 스택)에 잠깐 맡겨 뒀다가 마지막에 그 자리로
     * 그대로 옮겨 적는다. rbx는 rip/rspOld를 잠깐 담는 2차 스크래치로
     * 쓴다 - rbx의 원래 값은 그보다 먼저 이미 블록에 안전하게
     * 적어놨으므로 그 다음부터는 자유롭게 재사용해도 된다. 이 둘
     * 말고는 어떤 레지스터도 건드리지 않는다. */
    push rax                    /* 스택 A에 원래 rax를 잠깐 맡겨 둔다 */
    mov rax, [rdi]              /* rax = *oldTcbSlot = 이 Task의 고정 TaskTcb 블록 주소 */
    mov [rax+8], rbx            /* T.rbx */
    mov [rax+16], rcx           /* T.rcx */
    mov [rax+24], rdx           /* T.rdx */
    mov [rax+32], rsi           /* T.rsi (newTcb 인자 값 그대로 - 원래 rsi 값) */
    mov [rax+40], rdi           /* T.rdi (oldTcbSlot 인자 값 그대로 - 원래 rdi 값) */
    mov [rax+48], rbp           /* T.rbp */
    mov [rax+56], r8            /* T.r8 */
    mov [rax+64], r9            /* T.r9 */
    mov [rax+72], r10           /* T.r10 */
    mov [rax+80], r11           /* T.r11 */
    mov [rax+88], r12           /* T.r12 */
    mov [rax+96], r13           /* T.r13 */
    mov [rax+104], r14          /* T.r14 */
    mov [rax+112], r15          /* T.r15 */
    mov qword ptr [rax+120], 0  /* T.vector(미사용) */
    mov qword ptr [rax+128], 0  /* T.errorCode(미사용) */
    mov rbx, [rsp+8]            /* rbx(이미 안전하게 저장됨) = 진입 시 [rsp+0](반환 주소) - 지금 rsp=진입시-8이라 +8 */
    mov [rax+136], rbx          /* T.rip */
    mov qword ptr [rax+144], 0x08  /* T.cs = kGdtKernelCodeSelector */
    pushfq                       /* 스택 A: [rsp+0]=현재 rflags */
    pop qword ptr [rax+152]      /* T.rflags <- 방금 push한 값, rsp는 push 이전으로 복귀 */
    /* [수정, 2026-09-20, PN-81E49523 2단계 - 실측으로 발견] rspOld는
     * "호출자가 반환 직후 갖게 될 rsp" **값 자체**를 계산해야 하는데,
     * 그 값이 스택 어딘가에 데이터로 미리 저장돼 있는 게 아니다 -
     * `mov rbx, [rsp+16]`은 그 주소에 우연히 있던 호출자의 다른
     * 스택 내용(쓰레기 값)을 읽어 왔을 뿐이었다. 이 Task가 나중에
     * 그 쓰레기 rspOld로 iretq되면 완전히 임의의 스택 위에서 실행을
     * 재개하게 돼(실측: 곧바로 Double Fault, rspOld=0으로 캡처된
     * 사례 확인) - 필요한 건 **주소 계산**(`lea`)이지 메모리 로드
     * (`mov`)가 아니다. */
    lea rbx, [rsp+16]            /* rbx = 주소(진입 시 rsp+8) = 호출자의 반환 후 rsp */
    mov [rax+160], rbx           /* T.rspOld */
    mov qword ptr [rax+168], 0x10  /* T.ssOld = kGdtKernelDataSelector */
    pop qword ptr [rax+0]        /* T.rax <- 맨 처음 push해 둔 원래 rax, rsp는 진입 시점으로 복귀 */

    mov rsp, rsi                 /* newTcb 블록 자체를 새 스택으로 삼는다 - 복사 없음 */
    jmp isr_common_epilogue

.global kContextSwitchFromISR
kContextSwitchFromISR:
    /* rdi=oldTcbSlot(TaskTcb**), rsi=newTcb(TaskTcb*), rdx=currentFrame
     * (TaskTcb*, 인터럽트가 트랩한 스택 위의 진짜 InterruptFrame).
     * 이 함수는 "지금 살아 있는 레지스터"를 보존할 필요가 없다(호출자
     * Scheduler::onTick()/onForcedMigration() 자신은 이 호출 이후를
     * 신경 쓰지 않는다 - current를 다른 Task로 완전히 넘기는 마지막
     * 동작) - currentFrame이 가리키는 176바이트를 *oldTcbSlot이
     * 가리키는 고정 블록으로 그대로 복사하기만 하면 된다. */
    mov r8, [rdi]                /* r8 = *oldTcbSlot = 이 Task의 고정 TaskTcb 블록 주소 */
    mov r9, [rdx+0]
    mov [r8+0], r9
    mov r9, [rdx+8]
    mov [r8+8], r9
    mov r9, [rdx+16]
    mov [r8+16], r9
    mov r9, [rdx+24]
    mov [r8+24], r9
    mov r9, [rdx+32]
    mov [r8+32], r9
    mov r9, [rdx+40]
    mov [r8+40], r9
    mov r9, [rdx+48]
    mov [r8+48], r9
    mov r9, [rdx+56]
    mov [r8+56], r9
    mov r9, [rdx+64]
    mov [r8+64], r9
    mov r9, [rdx+72]
    mov [r8+72], r9
    mov r9, [rdx+80]
    mov [r8+80], r9
    mov r9, [rdx+88]
    mov [r8+88], r9
    mov r9, [rdx+96]
    mov [r8+96], r9
    mov r9, [rdx+104]
    mov [r8+104], r9
    mov r9, [rdx+112]
    mov [r8+112], r9
    mov r9, [rdx+120]
    mov [r8+120], r9
    mov r9, [rdx+128]
    mov [r8+128], r9
    mov r9, [rdx+136]
    mov [r8+136], r9
    mov r9, [rdx+144]
    mov [r8+144], r9
    mov r9, [rdx+152]
    mov [r8+152], r9
    mov r9, [rdx+160]
    mov [r8+160], r9
    mov r9, [rdx+168]
    mov [r8+168], r9

    mov rsp, rsi
    jmp isr_common_epilogue

/* Task::init()이 이 Task 전용 고정 TaskTcb 블록(Slab 할당, task.cpp
 * 참고)을 "이 함수로 막 돌아온 것처럼" 채워 둔다(kContextSwitch/
 * kContextSwitchFromISR 둘 다 최종적으로 isr_common_epilogue를 거쳐
 * 이 지점에 도착한다는 전제). 진입 시 rbx=entry 함수 포인터,
 * r12=entry에 넘길 arg(둘 다 InterruptFrame 안 자리에서
 * isr_common_epilogue의 pop으로 이미 복원됨).
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
 * 코어 전용 고정 TaskTcb 블록을 "돌아오는 것처럼" 채워 둔 걸 이
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
