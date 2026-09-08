/*
 * arch_context_switch(uint64_t* old_rsp_out, uint64_t new_rsp,
 *                      uint64_t new_pml4_phys) —
 * docs/spec/scheduler.md, kernel/core/sched/scheduler.cpp가 부르는
 * 커널 전용 훅(ADR-002 HAL 경계 패턴 — arch_irq_save/restore와 동일).
 *
 * SysV x86-64 콜리세이브 레지스터(rbp, rbx, r12~r15)만 스택에 저장하면
 * 충분하다 — 나머지(caller-saved)는 호출자가 이미 자기 책임으로
 * 처리했거나 신경 쓰지 않는 값들이다. 현재 rsp를 *old_rsp_out에 적어
 * 두고, new_rsp로 스택을 바꾼 뒤 저장된 콜리세이브 레지스터를 복원하고
 * ret한다 — new_rsp가 가리키는 스택은 (a) 예전에 이 함수가 다른
 * 스레드로부터 전환하며 저장해 둔 것이거나, (b) sched::create_kernel_thread/
 * create_user_thread가 손으로 흉내 내 만든 "처음 실행되는 스레드"용
 * 스택이다. 두 경우 모두 이 함수의 관점에서는 구분할 필요가 없다 —
 * 그냥 저장된 순서대로 되읽고 ret할 뿐이다.
 *
 * new_pml4_phys(M8 추가, 3번째 인자 rdx): 0이면 CR3를 건드리지 않는다
 * (지금까지의 모든 커널 스레드 — 전부 부팅 때 만든 같은 PML4를 공유
 * 하므로 굳이 다시 적재해 TLB를 무의미하게 비울 이유가 없다). 0이
 * 아니면 그 물리주소를 CR3에 적재한다 — 유저 스레드(object::thread::
 * owner_space != nullptr)로 전환할 때 sched.cpp가 owner_space->
 * page_table_root를 그대로 넘긴다.
 *
 * FPU(M9 ADR-127 → M11b ADR-133 개정): M9는 여기서 매 스위치마다
 * 무조건 FXSAVE/FXRSTOR했다(eager, 4·5번째 인자로 두 스레드의
 * fxsave_area를 받았다). M11b부터는 이 함수가 FPU 레지스터 내용을
 * 전혀 건드리지 않는다 — 대신 스위치마다 CR0.TS(bit3)만 무조건
 * 켠다. 이후 어느 스레드든 FPU/SSE/AVX 명령을 실행하는 순간 CPU가
 * `#NM`을 내고, 그 핸들러(fpu.cpp::arch_x86_64_handle_nm_trap,
 * idt.cpp가 벡터 7로 라우팅)가 실제 저장/복원을 담당한다 — "레지스터의
 * 실제 소유자가 바뀔 때만" 하므로, 소유자가 안 바뀌었으면(같은
 * 스레드가 다음 스위치 전까지 유일하게 FPU를 썼으면) 그 핸들러조차
 * 저장/복원 없이 즉시 리턴한다. 이 함수 자신은 그 구분을 전혀 몰라도
 * 되므로 인자에서 fxsave_area 포인터 두 개가 완전히 빠졌다 —
 * kernel/core/sched는 이제 FPU 버퍼 주소를 이 함수에 넘길 필요가
 * 없다(scheduler.cpp도 함께 갱신됨).
 */
.section .text
.global arch_context_switch
.type arch_context_switch, @function
arch_context_switch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, (%rdi)   /* *old_rsp_out = 현재 rsp */
    mov %rsi, %rsp     /* rsp = new_rsp */

    test %rdx, %rdx
    jz 1f
    mov %rdx, %cr3
1:
    mov %cr0, %rax
    or $8, %rax        /* CR0.TS(bit3) — 다음 FPU/SSE/AVX 명령에서 #NM 유발 */
    mov %rax, %cr0

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
