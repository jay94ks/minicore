# PN-584DB994 잔존 원인 확정 - 중첩 인터럽트의 실제 커널 스택 소진(8KiB 고정) - 수정 방향 확인 요청

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-53B93BFF
  status: approved
  updatedAt: 2026-09-20T20:16:29.647Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 배경

PN-584DB994(간헐적 SMP kPanic)의 재진입/상태불일치 부류는
DC-06FC78E8(전용 재진입 플래그, commit 2c4d9f7)과 설계자 직접 지시로
구현한 `Scheduler::captureCurrentFrame()`(인터럽트 발생 즉시 현재
Task tcb에 프레임 캡처, 중첩이 아닐 때만) 두 fix로 해소된 것으로
보인다 - 그 fix들 이후 "gCurrentTask가 실제 폴트 스택과 무관한
Task를 가리킨다"는 특유의 상태불일치 패턴이 더 이상 관측되지
않는다. 그러나 크래시 자체는 계속 재현되며, 남은 표본 전부가 다른
하나의 공통 패턴으로 수렴한다 - 이 DC는 그 패턴과 제안하는 수정
방향에 대한 설계자 확인을 요청한다.

## 확정된 잔존 패턴 - 중첩 인터럽트의 실제 스택 소진

`scripts/pn584_alloc_watch.py`의 스택 범위 진단(`fault_rsp`를 그
Task 자신의 `kernelStackTop-kernelStackSize`와 비교)으로 최근 확보한
3개 표본:

1. `interrupt_depth=3`, `headroom=-8568바이트`(스택 바닥보다 8568
   바이트 아래), `fault_rip`이 `gReactorTcbStorage+7`(.bss 데이터를
   명령어로 페치 시도).
2. `interrupt_depth=1`(중첩 아님), `headroom=-58328바이트`(task는
   `&gInitThread`) - 아래 "해석" 참고.
3. `interrupt_depth=4`, `headroom=-8728바이트`, `cr2==fault_rip`
   (코드 페치 폴트 확정)이고 상위 32비트를 `0xffffffff`로 복원하면
   `kernel::Lapic::id()+57`(PN-584DB994 갱신21이 이미 찾아낸 그
   주소) - 이 오래된 절단 패턴도 결국 스택 소진→반환주소 슬롯 손상
   이었음이 이제 명확해짐.

**핵심 단서**: `interrupt_depth`가 2 이상인 표본(1, 3번)의 headroom
적자가 공통적으로 "8KiB 남짓"(-8568, -8728) 규모다 - **중첩 인터럽트
한 단계가 실제로 소비하는 스택 공간이 이 커널의
`kTaskDefaultKernelStackSize`(고정 8KiB, `task.h:65`) 예산을 몇 단계
안에서 넘긴다**는 뜻이다. `InterruptFrame`(176바이트) + 각 ISR
분기의 호출 스택(`kIsrHandler`→`Scheduler::onTick()`/기타 핸들러→
그 안의 여러 함수 호출) + 로거 호출 등이 겹겹이 쌓이면, 3-4단계
중첩만으로도 8KiB를 실제로 초과할 수 있다는 정황이다.

**표본 2(비중첩인데 큰 폭 초과) 해석**: 이 가설과 모순되지 않는다 -
오히려 "한 번 스택이 실제로 넘친 뒤, 그 손상된(스택 밖을 가리키는)
`rsp`가 그 Task의 tcb에 실려(이제는 `captureCurrentFrame()`이
충실하게 매 인터럽트마다 그대로 저장하므로) 다음 디스패치들로 계속
전파/누적된다"는 2차 효과로 자연스럽게 설명된다 - 최초 손상은 아마
`interrupt_depth`가 높았던 어느 시점에 발생했고, 이 표본은 그 이후
같은 Task가 다시 논-중첩 상태에서 인터럽트당했을 때 이미 망가진
`rsp`를 그대로 다시 관측한 것으로 보인다.

## 확인 요청 - 수정 방향

**(A) 커널 스택 크기 확대** - `kTaskDefaultKernelStackSize`(현재
8KiB)를 늘린다(예: 16KiB 또는 32KiB). 가장 단순하지만, 근본적으로
"중첩이 계속 깊어지면 언젠가 또 넘칠 수 있다"는 문제 자체는 남는다
(상한을 늦출 뿐).

**(B) 전용 인터럽트 스택(IST) 도입** - x86_64의 IST(Interrupt Stack
Table) 메커니즘을 활용해, 인터럽트/예외 처리 자체는 Task 자신의
커널 스택이 아니라 코어별 전용 인터럽트 스택 위에서 실행되도록
구조를 바꾼다(Linux 등 성숙한 커널의 표준 접근) - Task의 커널
스택은 그 Task의 "정상 실행 흐름"만 쓰고, 인터럽트/중첩 인터럽트가
아무리 깊어져도 Task 스택을 전혀 건드리지 않는다. 근본적으로
안전하지만 설계 범위가 크다(RM-23F4B687/SP-677210E6의 기존 TSS/IST
서브시스템과의 통합 필요 - 이미 #DF/#MC 등 일부 벡터는 IST를 쓰고
있을 수 있어 확인 필요).

**(C) 중첩 자체를 줄인다** - 예를 들어 `onTick()`/`onForcedMigration()`
등 스케줄러 관련 ISR이 실행되는 동안은 (B)만큼 근본적이진 않아도)
일정 구간에서 인터럽트를 의도적으로 더 짧게/덜 중첩되게 만드는
정책적 완화(예: EOI를 보내는 시점을 조정하거나, 특정 벡터의 우선순위
조정) - 다만 이 프로젝트가 이미 "EOI를 늦추면 다음 틱이 아예 안
온다"는 이유로 이른 EOI를 채택한 배경이 있어(DC-06FC78E8 참고),
이 방향은 그 트레이드오프와 다시 충돌할 수 있다.

(A)/(B)/(C) 중 어느 방향을 원하는지, 혹은 다른 방향(예: A+B 조합)을
원하는지 설계자 확인 요청. 확정되면 별도 세션에서 구현 + 표준
4시나리오 회귀 + 기존 gdb 헌트 도구(`pn584_alloc_watch.py`의 스택
범위 진단)로 재현 안 됨(또는 현저히 감소)을 검증하겠습니다.

## 참고
- PN-584DB994(scheduled) - 이 확정 과정 전체가 기록된 조사 계획.
- DC-06FC78E8(approved) - 재진입 플래그 fix(선행 조치, 이미 발행).
- `task.h:65`(`kTaskDefaultKernelStackSize`), `scheduler.cpp`
  (`Scheduler::captureCurrentFrame`), `deferred_destruction.h/.cpp`
  (`kCurrentInterruptDepth`), `idt.cpp`(`kIsrHandler` 상단의 캡처
  호출).
- `scripts/pn584_alloc_watch.py`/`pn584_alloc_hunt2.sh` - 이 조사에
  쓴 gdb 계측 도구(전부 커밋됨) - 수정 후 재검증에 그대로 재사용
  가능(스택 범위/headroom 진단이 이미 있음).
