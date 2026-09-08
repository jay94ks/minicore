# 완료 보고: kernel-bootstrap M5 — 컨텍스트 스위치 + 스케줄러 골격

**대상 계획**: [kernel-bootstrap.md](../plan/kernel-bootstrap.md) §M5
**관련 스펙**: [scheduler.md](../spec/scheduler.md) §1~3
**관련 결정**: ADR-002, 014, 033, 034, 035, 042, 118
**실행일**: 2026-09-08

## 완료 기준 달성 확인

M5의 목표는 명시적이다: "커널 스레드 2개가 번갈아 실행됨을 시리얼
로그로 확인." QEMU 실행으로 확인했다.

```
cmake --preset x86_64-clang -S . -B build/x86_64-clang
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh
# => 20개 항목 모두 PASS (M1~M4 14개 + M5 6개: A/B 스레드가
#    iteration 0~2를 번갈아 찍음)
```

실제 QEMU 출력(발췌, 정확히 이 순서로 나온다):

```
[sched] create_kernel_thread a=1 b=1
[sched] thread A iteration 0
[sched] thread B iteration 0
[sched] thread A iteration 1
[sched] thread B iteration 1
[sched] thread A iteration 2
[sched] thread B iteration 2
[sched] thread A done
```

("thread B done"이 안 나오는 이유는 정상이다 — 아래 "검증 결과"
참고.)

## 수행한 작업

### 1. kernel/core/sched — run_queue + 협조적 라운드로빈 (scheduler.md §1~3)

- [kernel/core/sched/scheduler.hpp](../../kernel/core/sched/scheduler.hpp)/[.cpp](../../kernel/core/sched/scheduler.cpp) —
  `run_queue`(spinlock + kernel_band/user_band, 스펙 그대로),
  `sched::init/create_kernel_thread/enqueue/start/yield/current`.
- **협조적(cooperative) 전환만 구현했다** — 타이머 인터럽트로
  강제 선점하려면 IDT/APIC가 필요한데 M1~M8의 어느 마일스톤에도
  IDT 구축이 명시적으로 없다. 스레드가 `sched::yield()`를 직접
  호출해야 다음 스레드로 넘어간다. M5의 완료 기준("번갈아 실행됨")은
  이 방식으로도 충분히 관찰 가능하다 — 실제 선점형 스케줄링은
  인터럽트 인프라가 생기는 이후 마일스톤의 몫으로 명시적으로 미룬다.
- `object::thread`에 `context_rsp` 필드를 추가했다(M4가 만든
  구조체 확장) — "재개 시 이어서 실행할 지점의 스택 포인터"라는
  의미만 arch 독립적으로 규정하고, 실제 스택 레이아웃 해석은
  arch가 전담한다(ADR-002).
- **알려진 단순화**: 커널 스레드 스택(16KiB)은 정식 "커널 스택 슬롯
  영역"(2GiB, 가드 페이지, virtual-memory-layout.md §2)에 매핑하지
  않고 physmap 가상주소를 그대로 쓴다 — 지금은 모든 커널 스레드가
  부팅 때 만든 동일한 주소공간에서 돌기 때문에 충분하다. 가드
  페이지(스택 오버플로 조기 감지)는 필요해지면 추가한다.
  virtual-memory-layout.md §6이 "정확한 스택 크기는 M5가 정한다"고
  미뤄뒀던 것을 16KiB로 잠정 확정했다.

### 2. kernel/arch/x86_64/context_switch.S — arch_context_switch 훅

[kernel/arch/x86_64/context_switch.S](../../kernel/arch/x86_64/context_switch.S) —
SysV x86-64 콜리세이브 레지스터(rbp/rbx/r12~r15) 6개만 스택에
push/pop하는 고전적 "swtch" 기법. 새 스레드는
`sched::create_kernel_thread`가 스택을 손으로 이 레이아웃에 맞게
미리 채워둬(콜리세이브 6개 자리 + 진입점 주소), 최초 진입도 이미
한 번 잠들었다 깨어나는 스레드와 완전히 같은 경로(`ret`)로 처리된다
— 별도의 "첫 실행" 특수 케이스가 없다.

## 실행 중 재확인한 결정(ADR)

- ADR-118 관련 사항은 이번엔 **재발하지 않았다** — `run_queue` 배열을
  M4에서 확립한 패턴(전역 대신 `mm::alloc_pages` + placement new)을
  처음부터 따라 만들었고, 빌드 후 `nm | grep GLOBAL__sub_I`로
  빈 것을 확인했다.

## 검증 결과 (정직하게 보고)

- **확인함**: 스레드 A/B가 정확히 A0,B0,A1,B1,A2,B2 순서로 번갈아
  실행됨(반복 실행해도 동일). 이는 `pick_next_locked`가 FIFO로
  큐에서 빼고 `yield()`가 현재 스레드를 큐 뒤에 다시 넣는 조합이
  올바른 라운드로빈을 만든다는 것을 실행으로 증명한다.
- **"thread B done"이 로그에 없는 것은 버그가 아니라 예상된 동작이다**:
  A가 세 번째 `yield()`(i=2 반복 안에서 호출됨)까지 마친 뒤 루프를
  빠져나와 "A done"을 찍고 무한 `hlt` 루프로 들어간다 — 그 시점부터
  아무도 `yield()`를 다시 호출하지 않으므로(협조적 스케줄링,
  타이머 없음) B는 run_queue에 남은 채 영원히 다시 스케줄되지
  않는다. 이는 "협조적 전환만 구현한다"는 이 마일스톤의 설계
  그대로다.
- **확인하지 못함**: 노드가 2개 이상인 환경에서 스레드가 실제로
  다른 노드의 run_queue로 가는지 — M1~M8은 노드 1개(ADR-035)라
  `preferred_node % g_node_count`가 항상 0으로 접힌다. 코드는 노드별
  배열을 전제로 작성했지만 다중 노드 실행 자체가 검증 불가능하다.
- **확인하지 못함**: 커널 밴드(`priority_band::kernel`)와 유저
  밴드(`priority_band::user`)의 우선순위 분리 — 데모 스레드 둘 다
  `kernel` 밴드라 `user_band`가 실제로 후순위로 밀리는지는 실행으로
  보이지 않았다. `pick_next_locked`가 `kernel_band`를 먼저 보는
  코드 자체는 스펙 §3 순서를 그대로 따른다(코드 리뷰 수준 확인).

## 다음 마일스톤과의 접점

- M6(IPC)가 도네이션(scheduler.md §6, `boost_level` 일시 교체)을
  구현하려면 `sched::yield()`가 아니라 "특정 스레드를 지목해 즉시
  전환"하는 API가 추가로 필요하다 — 지금의 `yield()`는 "다음 차례"만
  고를 수 있다.
- 실제 선점형 스케줄링(타이머 인터럽트 기반)은 IDT/APIC 구축이
  선행되어야 한다 — 아직 어느 마일스톤에도 배정되어 있지 않다.
- §4(승격 비례 타임슬라이스)·§5(승격 syscall)는 `thread_sched_fields.boost_level`
  필드가 이미 있으니 값을 실제로 읽어 타임슬라이스/순서에 반영하는
  로직만 추가하면 된다.
