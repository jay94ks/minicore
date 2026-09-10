# 완료 보고: real-libc-syscall-layer M34 — 진짜 멀티코어 선점형 스케줄러 (OPEN-63 해소)

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M34
**관련 결정**: [kernel-scheduler.md](../design/kernel-scheduler.md) ADR-185, ADR-191, ADR-209;
[kernel-memory.md](../design/kernel-memory.md) ADR-136(갱신)
**실행일**: 2026-09-10

## 완료한 것

1. **`g_current`를 코어별 배열로** — `scheduler.cpp`의 `g_current[kern::mm::k_max_cpus]`.
   신규 `arch_current_cpu_index()`(smp.cpp, `arch_current_node_id()`와
   완전히 같은 HAL 경계 패턴)로 색인한다. `start()`/`yield()`/
   `block()`/`exit()`/`current()`/`on_timer_tick()` 전부 이 인덱스로
   자기 코어의 슬롯만 건드리도록 수정했다.
2. **AP는 유저 밴드에만 참여** — `pick_next_with_stealing()`이
   커널 밴드는 `cpu_index==0`(BSP)일 때만 본다. M21~M33의 커널 밴드
   데모 스레드들은 멀티코어 동시 실행을 검증한 적이 없어, 이
   라운드의 목표와 무관한 위험을 새로 만들지 않기 위한 의도적
   범위 좁힘이다.
3. **AP 전용 진입점 `start_ap()`(신규)** — `mark_multicore_ready()`
   (BSP가 자기 부트스트랩을 마친 뒤 딱 한 번 호출)/
   `wait_for_multicore_ready()`(AP가 그 신호를 기다린다, 신규
   `arch_wait_for_interrupt()` — 단발 HLT, idle.S)로 안전한 시점까지
   대기한 뒤, 아직 유저 스레드가 없어도 PANIC 대신 인터럽트로 깰
   때마다 재시도한다.
4. AP도 이제 `install_syscall_entry()`/`lapic_start_periodic_timer()`
   /`init_tss()`를 자기 몫으로 호출한다(`init_idt()`/`init_fpu()`와
   같은 "코어별 상태는 코어마다" 패턴).
5. `spawn_preempt_demo_processes()`가 `online_cpu_count()`쌍의
   busy+counter를 스폰한다 — 코어마다 뽑아 갈 유저 스레드가 있어야
   "AP에서 실제로 돈다"를 증명할 수 있다.

## 실행 중 발견 — 진짜 멀티코어 버그 3건

M21~M33까지 BSP 하나만 유저 스레드를 실행했기 때문에 절대 드러날 수
없었던, "이 상태는 코어마다 하나씩이어야 한다"는 종류의 버그를 세
번 연속으로 만났다. 셋 다 QEMU에서 실제로 재현·수정·재검증했다
(자세한 내용은 [ADR-209](../design/kernel-scheduler.md) 참고):

1. **`libk::irq_safe<Lock>` 자체의 데이터 경합.** 저장된 RFLAGS
   상태(`state_`)가 락 인스턴스 하나에 필드 하나뿐인데, 뮤텍스를
   잡기 **전에** 그 필드에 썼다 — 두 코어가 동시에 같은 락(예:
   `run_queue::lock`)을 다투면 서로의 저장 값을 덮어써, 한 코어가
   IF=1이어야 할 자리에 IF=0을 복원해 다시는 타이머로 깨어나지
   못하고 멈췄다. 뮤텍스를 먼저 잡고 그 다음에 상태를 저장하도록
   순서를 바꿔 고쳤다 — 그 시점부터는 배타적 소유자만 그 필드를
   건드리므로 경합이 없다.
2. **SYSCALL 진입이 읽는 커널 스택 포인터가 전역 하나였다**
   (`g_syscall_kernel_rsp`, M12/ADR-141). 코어별 슬롯 배열로 바꾸고,
   `swapgs`+`IA32_KERNEL_GS_BASE`(코어마다 자기 슬롯 주소)로 매
   SYSCALL 진입마다 정확히 "지금 이 코어"의 슬롯만 찾아가게 했다.
3. **TSS/GDT가 전역 하나였다** — x86_64 하드웨어는 코어마다 별도의
   TSS를 요구한다(TR이 가리키는 TSS에서 RSP0를 읽어야 ring3→ring0
   전환이 된다). AP가 자기 몫의 `init_tss()`를 부른 적이 없어 TR이
   무효 상태였고, AP에서 유저 스레드가 첫 인터럽트(자기 LAPIC 타이머
   틱)를 받는 순간 TSS를 못 찾아 그 코어가 죽었다(트리플 폴트로
   보이는 조용한 정지). 코어별 사설 GDT+TSS로 바꾸고 각 코어가
   자기 자신에게만 LTR하게 고쳤다.

세 버그는 순서대로 하나씩 드러났다 — (1)을 고친 뒤에도 동일한 정지
증상이 재현돼 (2)/(3)이 별개의 원인임을 QEMU로 재확인했다.

## 검증

x86_64 전체 재빌드 성공. QEMU(`MINICORE_QEMU_SMP=4`)에서 확인: BSP의
커널 밴드 데모(thread A/B, fpu 데모)와 AP 3개가 각자 고른
`preempt_busy`/`preempt_counter` 유저 프로세스가 **실제로 인터리빙
되어 함께 진행**됨을 로그로 확인했다(`[sched] cpuN start_ap picked
first user thread` 이후 `[preempt-demo] counter=100000...` 과
`[sched] thread A iteration 1` 등이 섞여서 나옴 — 어느 한쪽이 다른
쪽을 굶기지 않는다). QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0) — 4코어 전부 유저 스레드 실행 확인 |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0) |

## 남겨 둔 것

real-libc-syscall-layer.md M35(musl locale)부터 계속 진행한다.
`timer_source_interface`(ADR-191)는 여전히 만들지 않았다 — 소비자가
BSP+AP뿐이라 지금은 조기 추상화로 판단, aarch64 이식 등 새 타이머
백엔드가 실제로 필요해지는 시점에 재검토(ADR-208/209). `handle_table`
의 무동기화 상태(ADR-136이 M11부터 이미 지적)는 이번 라운드도
해소하지 않는다 — busy/counter 데모는 IPC를 쓰지 않아 이 경로를
건드리지 않았다(OPEN-68로 새로 등록, M37 pthread 또는 그 이전 실사용
시점에 재검토).
