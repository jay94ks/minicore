# HPET 없는 환경에서 스케줄러 LAPIC 틱과 Timer 전역 틱의 하드웨어 소유권 충돌

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-0CC88ABB
  status: review
  updatedAt: 2026-09-14T07:35:55.989Z
  갱신: node scripts/export-cnw-docs.mjs
-->
## 배경

PL-2D3184BC 5단계(LAPIC 틱 기반 선점) 구현 중 발견 - 스케줄러 완료
작업(SP-04EE2A18/SP-1FBC0EEB 구현을 위한 선행조건) 도중 실측 코드
리뷰로 찾은 설계 공백이다.

- `Timer::init()`(timer.cpp)은 HPET가 없으면 기존부터 `Lapic::
  startPeriodicTimer(kTimerVector, 100)`으로 **물리 LAPIC 주기
  타이머**를 직접 점유해 전역 `Timer::tickCount()`를 공급해 왔다.
- 이번에 PL-2D3184BC 7절("선점 결정은 코어별 독립 LAPIC 타이머가
  담당")에 따라 신설한 `Scheduler::startTickOnThisCore()`도 **같은
  물리 LAPIC 타이머**를 `kSchedulerTickVector`로 재프로그램한다.
- 코어당 물리 LAPIC 타이머는 하나뿐이라, 한 코어에서 두 벡터를
  동시에 낼 수 없다 - `Lapic::startPeriodicTimer`를 두 번 부르면
  나중 호출이 이전 설정을 그냥 덮어쓴다.
- HPET가 있는 환경(이 프로젝트의 QEMU 개발 환경 포함 - 지금까지의
  모든 실측이 이 경로)에서는 `Timer::init()`이 LAPIC을 아예 안
  건드리므로 문제가 드러나지 않는다. **HPET가 없는 폴백 환경에서만**
  `Scheduler::startTickOnThisCore()` 호출이 `Timer`가 먼저 걸어 둔
  LAPIC 프로그래밍을 조용히 덮어써 전역 `Timer::tickCount()`가 더
  이상 증가하지 않게 된다(스케줄러 자체의 선점/디스패치는 정상
  동작 - 영향받는 건 오직 전역 시각 카운터).

## 결정이 필요한 사항

1. **HPET 없는 환경에서 전역 tickCount()를 계속 공급할 방법**:
   - (a) `Scheduler::onTick`이 이 경우 한정으로(예: BSP 코어에서만,
     SMP에서 코어 수만큼 중복 카운트되지 않도록) `Timer::onTick()`도
     같이 호출해 대신 공급한다.
   - (b) `Timer::init()`을 "HPET 없으면 LAPIC 폴백 없이 tickCount는
     그냥 0에 머무른다"로 단순화하고, 이 v1 범위에서는 HPET 없는
     하드웨어의 전역 시각 기능 자체를 지원 안 하는 것으로 문서화한다.
   - (c) 그 환경만 별도의 저속 폴링 시간원(예: 레거시 PIT IRQ0,
     `Timer::enableLegacyPitIrq()`가 이미 있음)으로 전역 시각을
     대신 공급한다.
2. 위 어느 쪽이든, **SMP 환경에서 "전역 카운터를 중복 없이 누가
   증가시킬지"**를 명확히 해야 한다(각 코어가 독립적으로 자기 LAPIC
   틱을 갖는 이상, 특정 코어 하나만 대표로 증가시켜야 함).

## 참고

- PL-2D3184BC - 이 결정이 반영될 스케줄러 실행 계획(5/7단계).
- timer.cpp/lapic.h - 충돌하는 두 소비자(Timer::init()의 LAPIC 폴백,
  Scheduler::startTickOnThisCore()).
- 현재 코드는 이 문제를 아직 반영하지 않은 상태다(HPET가 있는 개발/
  실측 환경에는 영향이 없어 당장 막힌 경로는 아님).
