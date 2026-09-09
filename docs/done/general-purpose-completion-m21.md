# 완료 보고: general-purpose-completion M21 — 선점형 스케줄링

**대상 계획**: [general-purpose-completion.md](../plan/general-purpose-completion.md) §M21
**관련 결정**: [kernel-scheduler.md](../design/kernel-scheduler.md)
ADR-176(LAPIC 타이머 기반 선점형 스케줄링), ADR-177(TSS.RSP0
스레드별 분리 — ADR-176이 드러낸 별도 결함의 수정)
**실행일**: 2026-09-10

## 완료한 것

### D0. 구현 — LAPIC 타이머 + 새 IDT 벡터 + `sched::on_timer_tick()`

- [lapic.hpp/.cpp](../../kernel/arch/x86_64/lapic.hpp)에 LVT
  Timer(0x320)/Initial Count(0x380)/Divide Config(0x3E0) 레지스터를
  추가하고 `lapic_start_periodic_timer(vector, initial_count)`를
  구현했다(divide by 16, periodic 모드). **BSP에서만** 호출한다 —
  AP는 M10/M11 결정대로 여전히 run_queue에 참여하지 않는다.
- [idt.hpp](../../kernel/arch/x86_64/idt.hpp)에 `k_vector_timer`
  (0x40)를 추가하고, [idt.cpp](../../kernel/arch/x86_64/idt.cpp)의
  `interrupt_dispatch()`가 **EOI를 먼저 보낸 뒤** `sched::on_timer_tick()`
  을 부르도록 라우팅했다(순서가 중요한 이유는 ADR-176 본문 참고 —
  `on_timer_tick()`이 `yield()`로 다른 스레드의 콜스택으로 넘어갈 수
  있어, "EOI 나중에"였다면 이 코어가 그 사이 다른 인터럽트를 못
  받는다).
- [kernel_objects.hpp](../../kernel/core/object/kernel_objects.hpp)에
  `object::thread::preempt_ticks_remaining` 필드를 추가했다.
  [scheduler.cpp](../../kernel/core/sched/scheduler.cpp)에
  `slice_multiplier(boost_level)`/`ticks_for(sched_fields)`/
  `reset_preempt_budget(thread&)`/`on_timer_tick()`을 추가하고,
  `start()`/`yield()`(양쪽 분기)/`block()`/`exit()` 전부에서
  `g_current`가 바뀌는 지점마다 `reset_preempt_budget()`을 부르게
  했다. `create_kernel_thread`/`create_user_thread`/
  `create_forked_thread` 셋 다 `sched.base_time_slice_us`를 기본값
  (20틱)으로 채운다 — scheduler.md §4가 M4부터 정의만 해 두고 한
  번도 읽힌 적 없던 `base_time_slice_us`/`boost_level`을 이 라운드가
  처음으로 실제 소비한다.
- [scheduler.hpp](../../kernel/core/sched/scheduler.hpp)의
  `run_queue::lock`을 `spinlock` → `irq_safe<spinlock>`(libk/irq_safe.hpp,
  ADR-076)로 승격했다 — 이 락을 쥔 코드가 이제 타이머 ISR에서도
  호출되기 때문(자세한 안전성 분석은 ADR-176 참고: 이 커널이 커널
  코드를 항상 IF=0으로 실행하는 현재 불변식 덕에 지금은 실제
  재진입 데드락이 없지만, 그 불변식에만 의존하지 않도록 방어적으로
  승격했다).

### D1. 실기(QEMU) 검증 데모 — `init/preempt_demo/`

계획의 검증 방법("무한루프만 도는 유저 스레드 하나와, 그 옆에서
자기 카운터를 늘리는 다른 스레드")을 위해 `init/preempt_demo/busy.c`
(busy-loop, 스스로 yield류 syscall을 전혀 안 부름)와
`init/preempt_demo/counter.c`(자기 카운터를 늘리다 20번마다
`sys_debug_log`로 보고, 완료 후 `sys_thread_exit`)를 새로 만들었다.
`init/initrun`과 같은 이유(top-level `CMakeLists.txt`가 kernel보다
먼저 빌드해야 initrd에 심을 수 있다)로 `libmc`를 못 써 syscall을
직접 감쌌다. `init/initrun/CMakeLists.txt`의 `mkinitrd.py` 호출에
`preempt_busy=`/`preempt_counter=` 두 엔트리를 추가했고,
`kernel_main.cpp::spawn_preempt_demo_processes()`가 이 둘을
`initrd::find_entry()`로 찾아 평범한 `process_spawn()`(handle
없음, `create_endpoint=false`)으로 띄운다 — `demo_sched()`가
`initrun`을 스폰한 직후 호출한다.

**QEMU 실측**: `tools/run-qemu.sh x86_64`로 부팅한 로그에서
`[preempt-demo] counter=100000` ~ `counter=2000000`(20줄)이 전부
찍혔다 — busy가 절대 스스로 CPU를 내놓지 않는데도 counter가
끝까지 진행됐다는 것 자체가 타이머 선점의 직접 증거다.

**무한루프가 아니다(중요한 수정)**: 처음에는 busy를 진짜 무한
루프로 짰다가, `tools/smoke-test-x86_64.sh`(virtio-blk 부트
디바이스가 붙는 전체 시스템 경로)를 돌려 보니 이후 모든 서비스가
영원히 CPU 1/N을 이 데모에 빼앗겨 120초 타임아웃 안에 셸 자체
테스트까지 못 갔다 — busy를 2억 회 반복 후 `sys_thread_exit`하는
유한 루프로 고쳐 해결했다(counter의 전체 실행보다 확실히 오래
걸리도록 여유를 크게 잡은 값, 보정 없음).

### D2. 발견한 버그와 수정 — ADR-177(TSS.RSP0 전역 공유)

D1의 첫 실행에서 `tools/run-qemu.sh` 경로가 `arch_x86_64::eoi()`
(전혀 무관한 함수) 안에서 `#PF`(vector=14, error_code=0x2)로
죽었다. 원인은 `init_tss()`(M12, ADR-143)가 `TSS.RSP0`를 **전역
스택 하나**로 고정해 둔 것 — "예외 처리는 항상 순차적, 처리 끝나면
곧바로 IRETQ"라는 그 시절의 전제가 이 마일스톤의 타이머 선점으로
깨진다(선점된 스레드의 IRETQ가 "곧바로"가 아니라 "나중에 다시
스케줄될 때"로 미뤄지는 사이, 다른 유저 스레드가 똑같은 전역 RSP0
꼭대기를 다시 밀어써 앞선 스레드의 보류 중인 인터럽트 프레임을
뭉갠다). `tss.hpp/.cpp::sync_exception_stack()`을 추가해 RSP0를
`syscall_kernel_rsp`(스레드별로 이미 있던 값)로 매 컨텍스트
스위치마다 맞추도록 고쳤다 — M12(ADR-141)가 `g_syscall_kernel_rsp`
를 전역에서 스레드별로 바꾼 것과 정확히 같은 문제, 같은 해법.
자세한 근거·재현 로그는 ADR-177 본문 참고.

### D3. 회귀 검증(4개 QEMU 스위트 전부)

수정 후 아래를 모두 재실행해 회귀가 없음을 확인했다(`MINICORE_QEMU_BIN`
을 로컬 QEMU 실행파일로 지정):

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh`(M1~M20, 46개 문자열 + 셀프테스트) | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-smp-x86_64.sh`(AP 기동/TLB shootdown) | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0개) |

GRUB Multiboot2/UEFI(OVMF) 실기 부팅 경로([real-hardware-boot-verification.md](real-hardware-boot-verification.md))
는 이번 라운드에서 다시 돌리지 않았다 — 이 마일스톤의 변경은
`kernel_main()`이 이미 부트로더로부터 제어를 넘겨받은 **이후**의
순수 커널 내부 로직(IDT/LAPIC/스케줄러)이라 부트로더 경로와
무관하고, GRUB/UEFI 각각의 고유 리스크(8259 PIC, EFI 스텁의
`arch_data_addr`)는 이 변경이 건드리지 않는다 — 다만 실제로 다시
확인한 것은 아니므로 "확인했다"고 주장하지 않는다(CLAUDE.md 실행
원칙).

## 남겨 둔 것 (OPEN)

- **OPEN-62**: LAPIC 타이머 주파수를 PIT/HPET로 보정하지 않았다 —
  `base_time_slice_us`는 이름과 달리 지금 "타이머 틱 수"로만
  쓰인다.
- **OPEN-63**: AP는 여전히 스케줄러에 참여하지 않는다 — 진짜
  멀티코어 선점형 스케줄러는 이 마일스톤 범위 밖이며, M22~M26
  어디에도 아직 배정돼 있지 않다.

둘 다 [open-items.md](../design/open-items.md)에 기록했다.

## 다음

[general-purpose-completion.md](../plan/general-purpose-completion.md)
§M22(프로세스 생명주기: wait/exit status/최소 시그널)로 이어간다.
