# Scheduler gNormalQueues 영구 기아 - Process-less KernelThread(devmgr/fs/AsyncReactor)가 vruntime을 절대 안 내면서 같은 큐에서 UserThread와 경쟁

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-EECFE2E0
  status: review
  updatedAt: 2026-09-27T19:12:59.396Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 배경

`PN-24A2B6F5`(authmgr E2E)/`PN-ECCAD541`(기각)/`PN-AA9D7030`(현재
조사)를 거치며 authtest+authmgr Ping-only 재현이 매번 "authtest가
Ready 상태로 영원히 디스패치 안 됨"으로 멈추는 걸 gdb 스냅샷 +
TEMP 브레드크럼(조사 후 전부 원복 완료, 표준 회귀 4종 클린)으로
집요하게 추적했다. 처음엔 크로스코어 데이터 레이스(`DC-C4A011C7`과
유사한 패턴), 그다음엔 큐 우선순위 역전을 의심했으나 둘 다 정확한
계측으로 기각했고, **이번에 100% 확정된 근본 원인**을 아래에 정리한다.

## 확정된 근본 원인 (계측으로 직접 확인, 정적 추론 아님)

1. **`Scheduler::pickNext(0)`은 매번 `gImmediateQueues[0]`/
   `gRtQueues[0]`를 확인하지만 23/23 재현에서 단 한 번도 히트하지
   않았다**(`fellThrough` 카운터가 매번 `calls`와 정확히 같음) -
   즉 매번 `gNormalQueues[0]`까지 정상적으로 도달한다. 이전 리비전이
   의심한 "즉시 큐 우선순위 역전" 가설은 기각.
2. **`gNormalQueues[0]`의 `minVruntime()`이 15초 내내(1800회 이상
   호출) 단 한 번도 `0`을 벗어나지 않았다** - 반면 authtest는 딱
   한 번 선점된 뒤 `vruntime=400`으로 재삽입됐다. `OrderedList::
   insert()`(선형 탐색, `Traits::keyOf(*item) < Traits::keyOf(*cur)`
   비교)는 코드 리뷰로 버그를 못 찾았다 - 정상적으로 동작하는 정렬
   삽입이 `popMin()`으로 항상 최솟값(0)을 내주고 있을 뿐이다.
3. **원인**: `Scheduler::onTick()`의 vruntime 적립 로직
   (`scheduler.cpp:1836-1838`)은

   ```cpp
   if (current->taskClass == TaskClass::Normal && current->isUserLevel) {
       current->vruntime += (kEffectiveWeightBase * kVruntimeScale) / kEffectiveWeightOf(current->weight);
       current->cpuTicksUsed += 1;
       ...
   }
   ```

   **`isUserLevel`이 `true`인 Task에만 vruntime을 적립한다.**
   그런데 `kSpawnKernelThread()`(task.cpp:304-314, devmgr/fs/
   AsyncReactor 리액터 Task가 전부 이 경로로 스폰됨)는
   `isKernelMode = true`만 세팅할 뿐 `isUserLevel`은 건드리지
   않는다 - `Task::isUserLevel`의 기본값은 `false`(task.h:277)다.
   반면 `Task::taskClass`의 기본값은 `TaskClass::Normal`(task.h:241)
   이고 `kSpawnKernelThread()`도 이를 바꾸지 않는다 - 즉 **devmgr/
   fs/코어별 AsyncReactor 리액터 KernelThread는 전부
   `TaskClass::Normal`이라 `Scheduler::enqueue()`를 통해
   UserThread와 똑같이 `gNormalQueues[coreIndex]`에 들어가 vruntime
   순으로 경쟁하지만, `onTick()`의 `isUserLevel` 조건 때문에 자기
   자신은 vruntime을 절대 적립하지 않는다.**

## 결과 (일반화된 영향)

이 큐에 `TaskClass::Normal`의 Process-less KernelThread가 하나라도
있으면(devmgr/fs는 부팅 시 항상 존재, 코어별 AsyncReactor 리액터
Task도 항상 존재 - 즉 **모든 코어의 `gNormalQueues`가 항상 이
조건에 해당한다**), 그 KernelThread는 vruntime이 영원히 초기값(0)에
머물러 `popMin()`에서 항상(또는 거의 항상) 최솟값으로 선택된다.
반대로 UserThread(authtest 등)는 한 번이라도 실제로 CPU를 써서
vruntime이 0보다 커지는 순간, **그 KernelThread(들)이 존재하는 한
원리적으로 다시는 선택되지 못할 수 있다** - 관측된 authtest 정지가
정확히 이 패턴이다(`vruntime=400` vs 영구히 `minVruntime=0`).

**이 문제는 authmgr/authtest에 국한되지 않는다** - devmgr/fs
KernelThread가 존재하는 한(즉 항상), 어떤 코어에서든 한 번이라도
선점당한 UserThread는 그 코어에 KernelThread가 함께 있으면 이후
영구 기아될 수 있다는 뜻이다. 이번 세션 앞서 겪었던 다른 설명 안
된 heisenbug들(예: 이전에 별도로 추적하던 산발적 무응답 증상들)
중 일부가 사실 이 근본 원인의 다른 발현이었을 가능성도 배제 못
한다 - 확인은 필요하지만 이 DC의 범위 밖으로 남긴다.

## 결정이 필요한 지점 - 고칠 방향

(A) **Process-less KernelThread도 vruntime을 적립하게 한다** -
    `onTick()`의 조건에서 `isUserLevel` 검사를 빼거나
    `taskClass==Normal`만으로 판단한다. 가장 간단하지만, "왜
    `isUserLevel` 조건이 원래 있었는지"(SP-B26CDBDD §3.2가 명시한
    설계 의도인 것으로 보임 - "커널 자신의 Normal Task까지 공정
    스케줄링/계정 대상으로 끌어들이지 않기 위함")를 뒤집는 것이라
    원래 그 결정의 근거를 재검토해야 한다.
(B) **Process-less KernelThread를 `gNormalQueues`(vruntime 경쟁)에서
    아예 빼고 별도 큐/메커니즘으로 스케줄한다** - 예를 들어 전용
    "커널 서비스 큐"를 신설하거나, `gRtQueues`처럼 단순 FIFO/우선순위
    큐로 옮긴다. UserThread와의 CPU 시간 경쟁 자체를 설계상 분리하는
    더 근본적인 해법이지만 범위가 크다(devmgr/fs/AsyncReactor 리액터
    Task 전부의 스케줄링 경로 변경).
(C) **`Task::taskClass`를 devmgr/fs/AsyncReactor 리액터에 한해
    `TaskClass::RealTime`(또는 새 전용 클래스)로 바꿔 `gRtQueues`로
    보낸다** - `enqueue()`가 이미 `taskClass`로 큐를 분기하므로
    최소 변경이지만, `gRtQueues`가 실제로 어떤 정책(FIFO? 우선순위?)
    인지, RT 클래스에 부여되는 다른 의미(선점 정책 등)와 충돌하지
    않는지 확인이 필요하다.
(D) **먼저 gdb/계측으로 이 근본 원인이 실제로 다른 과거
    heisenbug들과도 연결되는지 더 넓게 확인한 뒤 결정** - 범위가
    프로젝트 전체 스케줄러 공정성에 걸친 문제로 보여, 이번 authtest
    재현 하나만 보고 좁게 고치기보다 설계자가 먼저 전체 그림을
    보고 판단하고 싶을 수 있다는 점을 고려한 선택지.

개인적으로는 (A)가 가장 즉각적이고 위험이 적어 보이나(devmgr/fs가
실제로 유의미한 CPU를 거의 안 쓰므로 vruntime 적립을 켜도 그들
자신의 스케줄링에 큰 영향은 없을 가능성이 높음), `isUserLevel`
조건이 명시적 설계 의도(SP-B26CDBDD §3.2)였던 만큼 최종 방향은
설계자 판단에 맡긴다.

## [교차 확인 추가, 2026-09-28] 두 번째 독립 희생자 확인 - dbgdriver(PN-0556C759)도 같은 시그니처로 영구 정지

`PN-0556C759`(멀티스레드 하드웨어 브레이크포인트 재현) 재검증 시도
중, 그 재현 드라이버(`minicore/dbgdriver`)의 물리 UserThread가
`state=Ready, inRunQueue=true, vruntime=1024, cpuTicksUsed=1`로
authtest와 **정확히 동일한 시그니처**로 영구 정지하는 것을 gdb로
확인했다 - `vruntime` 값까지 완전히 같아, 같은 최초 선점 타이밍/
비용 구조에서 이 버그가 매우 일관되게 발동함을 시사한다. `PN-0556C759`
가 이번 세션 내내 관찰해 온 "재현율이 원래 33%에서 0%로 떨어졌다"는
현상 자체가, 원래 찾던 버그가 사라진 게 아니라 **이 gNormalQueues
기아가 그 재현 하네스를 먼저 잡아먹어 원래 코드 경로에 도달하지도
못하게 막고 있었을 가능성**을 뒷받침한다 - "authmgr에 국한되지
않는다"는 위 절의 판단을 독립적으로 뒷받침하는 두 번째 사례.

## 참고
- `PN-AA9D7030` - 이 근본 원인을 확정한 조사(23/23 재현, TEMP
  브레드크럼 전부 원복 완료).
- `PN-0556C759` - 두 번째 독립 희생자 확인 사례(dbgdriver).
- `PN-E4C6AF72`/`PN-0B461E6F`(둘 다 scheduled) - "레이아웃 민감
  heisenbug, 최근 재현 안 됨"으로 분류돼 온 다른 두 계획 - 아직
  직접 확인은 안 했지만(별도 재현 시도 비용이 커서 이번엔 보류),
  이 DC가 해소된 뒤 재현을 다시 시도해 "진짜 원인 해소"와 "이
  스케줄러 기아에 막혀 도달 못 함"을 구분할 가치가 있는 후보로
  교차 기록해 둔다 - 다만 이 둘의 원 증상(실제 크래시/PANIC)은
  authtest/dbgdriver의 증상(조용한 영구 정지)과 종류가 달라
  직접적 연관성은 dbgdriver 사례보다 약하다.
- `PN-24A2B6F5` - authmgr E2E 완성 계획, 이 DC의 결정에 의존.
- `PN-ECCAD541`(rejected) - 먼저 기각된 Channel 계층 가설.
- `DC-C4A011C7`(approved) - 이전에 고친 waitingTask 크로스코어
  레이스(같은 조사 초기 단계의 다른 가설, 실재하는 버그였으나 이
  증상의 원인은 아니었음).
- `minicore/kernel/scheduler.cpp:1836-1838` - vruntime 적립 조건
  (`isUserLevel` 검사).
- `minicore/kernel/task.cpp:304-314` - `kSpawnKernelThread()`,
  `isUserLevel`을 안 건드리는 지점.
- `minicore/kernel/task.h:241,277` - `Task::taskClass`/`isUserLevel`
  기본값.
- `SP-B26CDBDD` §3.2 - `isUserLevel` 조건의 원래 설계 근거로
  추정되는 문서(재확인 필요).
