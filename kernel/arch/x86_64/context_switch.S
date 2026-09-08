/*
 * arch_context_switch(uint64_t* old_rsp_out, uint64_t new_rsp,
 *                      uint64_t new_pml4_phys,
 *                      void* old_fxsave_area, void* new_fxsave_area) —
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
 * old_fxsave_area/new_fxsave_area(M9, ADR-127 — 4·5번째 인자 rcx/r8):
 * FXSAVE/FXRSTOR 대상인 object::thread::fxsave_area(512바이트, 16바이트
 * 정렬)의 주소. eager 정책 — 스위치마다 무조건 저장/복원한다(lazy
 * CR0.TS/#NM 트랩 전환은 ADR-133, M11b 이후). old_fxsave_area가
 * nullptr이면 저장을 건너뛴다 — sched::start()(부팅 후 첫 스위치,
 * "이전 스레드"라는 게 아직 없다)와 sched::exit()(빠져나가는 스레드는
 * 다시 스케줄되지 않으므로 그 FPU 상태를 저장할 이유가 없다)가 이
 * 경우다. new_fxsave_area는 항상 유효해야 한다(모든 실제 스레드가
 * fxsave_area를 갖고 있으므로).
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

    test %rcx, %rcx    /* old_fxsave_area == nullptr? */
    jz 1f
    fxsave (%rcx)
1:
    fxrstor (%r8)

    mov %rsi, %rsp     /* rsp = new_rsp */

    test %rdx, %rdx
    jz 2f
    mov %rdx, %cr3
2:

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
