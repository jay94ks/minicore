# Minicore 설계공백 검수

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: RM-F2DAFF66
  status: review
  updatedAt: 2026-09-18T13:07:35.680Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

﻿﻿# Minicore 설계공백 검수

설계자 지시(2026-09-17, 메시지) - "NUMA 관련 문서 찾아보고 반영된게
있는지 없는지 파악해" → "응 갭을 다 매꿔야해" → "계속 더 넓혀서
봐야하고, 전부 모아서 이 문서로 만들어놔. 루프에도 명시해서 다
찾아낼 수 있도록해." RM-7C249618/RM-C65F7760/RM-48E1E610/RM-B5764185/
RM-28225668와 같은 성격의 **현황판 문서** - 다만 저 문서들이 "무엇이
구현됐는지"를 추적한다면, 이 문서는 **"설계 문서가 확정했다고 적어
둔 것 중 실제 코드에 반영되지 않은 것"**을 추적한다.

## 왜 이 문서가 필요한가 - 발견 경위

`PL-2D3184BC`(스케줄러 설계)가 `Task` PCB가 가져야 할 필드 목록에
"소속 NUMA 노드"를 명시했는데, 실제 `task.h`에는 그 필드가 없었다
(minicore-f8 세션이 설계자 지시로 NUMA 문서를 찾다가 발견,
2026-09-17). 더 나쁜 건 그 설계의 출처 문서(`DC-8EA1E7F6`)가 "이
결정은 PL-2D3184BC에 전부 반영돼 실제 구현·검증까지 끝났다"고
**틀린 단언**을 archived 상태로 남기고 있었다는 것 - 이 프로젝트의
표준 관례(RM-* 현황판, CLAUDE.md 규칙 7-13)가 "무엇을 했는지"는
꼼꼼히 추적하지만, "설계 문서가 확정한 것 중 빠진 게 없는지"를
역방향으로 감사하는 절차는 없었다.

## 방법론 - 어디를 봐야 하는가

**결론(2026-09-17 기준 조사 경험)**: 이 프로젝트의 RM-* 현황판
(라이브러리/VFS/Syscall/Signal/인터럽트 벡터 목록)은 두 세션이
매 틱 갱신하며 실측과 맞춰 왔기 때문에 신뢰도가 높다 - 표 형태로
"번호/이름/상태"가 명시적으로 박혀 있어 빠뜨리기 어렵다. **위험
지대는 오히려 큰 SP/DC 문서 안의 산문(prose) 형태 "확정된 설계"
절이다** - 특히 여러 항목을 한 번에 나열하는 목록(예: "PCB가 가질
필드 후보: A, B, C, ..., Z") 안에서 마지막 몇 개 항목이 실제
구현 단계에서 조용히 누락되는 패턴이 두 번 확인됐다(§1 참고).

**점검 절차(반복 가능한 방법)**:
1. approved/archived 상태인 SP/DC 문서 중 "확정된 설계"/"결정"
   절이 여러 필드·단계·API를 **목록 형태로 나열**하는 문서를 고른다.
2. 그 목록의 항목 하나하나를 실제 소스(`docs git grep`/`git read`)
   와 대조한다 - 특히 목록 뒷부분(앞부분은 먼저 구현되고 검증되는
   경향이 있어 상대적으로 안전, 뒷부분이 누락 위험이 높다).
3. 문서가 "전부 반영/완료됐다"고 자체 선언한 문장을 발견하면 **그
   선언 자체를 의심하고 재검증**한다(DC-8EA1E7F6의 사례).
4. 실제로 빠진 게 확인되면: 코드 갭이면 PN 계획을 등록(CLAUDE.md
   규칙 7)하고 원본 문서에 정정 각주(RM-23F4B687 §4 관례 - 원문은
   보존, 정정만 추가), 설계 자체가 후속 결정으로 바뀌어 원문이
   단순히 낡은 것뿐이면(코드 갭 아님) 문서 정정만 하고 이 문서에
   "문서만 정정"으로 기록.

## §1. 확정된 발견 (완료)

### 1-A. `Task::numaNode` 필드 누락 (코드 갭, 완전 해소)

- **출처**: `PL-2D3184BC`(스케줄러 실행 계획, `DC-8EA1E7F6` 확정
  설계 반영) "Task 자료구조" 절의 PCB 필드 후보 목록 - "소속 NUMA
  노드"가 마지막 항목으로 나열돼 있었으나 실제 `task.h`엔 없었음.
- **조치**: **PN-A74871F2**(필드 반영, scheduled) 등록 -
  `Acpi::cpuNumaNode()`로 생성 코어의 노드를 기록. 이 필드를
  Push/Pull 로드밸런싱이 실제로 참고해야 하는지는 `SP-9525C4C0`에
  `QU-759C9C1C`로 확인 → 설계자 답변 **(A) 참고함**(같은 노드 우선
  이관) → `SP-9525C4C0` §2.3에 설계 반영 완료, **PN-9DDFB774**
  (Push/Pull 소비 로직, PN-A74871F2에 plan_depend)로 별도 추적.
- **부수 정정**: `DC-8EA1E7F6`(archived)의 "전부 반영·검증 끝났다"
  단언에 정정 각주 추가. `RM-32D06563`에 `Task::numaNode` 용어 등록.
- **[완전 해소, 2026-09-17, minicore-88 세션]** PN-A74871F2
  완료(commit 7347c47) - `Task::numaNode` 필드 반영 및 QEMU 단일/
  2노드 토폴로지 검증 완료(단, 부팅 시점 Task가 전부 BSP에서
  생성돼 `numaNode!=0` 실제 경로는 아직 미실측 - PN-A74871F2 본문
  참고).
- **[체인 완전 완료, 2026-09-17, minicore-88 세션]** PN-9DDFB774도
  완료(commit 9057d08) - `kFindLeastLoadedCoreNumaAware`/
  `kFindMostLoadedCoreNumaAware`로 Push/Pull이 실제로 같은 NUMA
  노드를 우선하도록 배선, 단일/2노드 토폴로지 무회귀 확인. 이걸로
  NUMA 갭 체인(발견→PN-A74871F2→PN-9DDFB774) 전부 완료 -
  1-A는 이제 완전히 닫힌 항목이다.

### 1-B. `SP-1FBC0EEB` Channel IPC `onCancel` 미구현 - 댕글링 포인터
위험 (코드 갭, **[완전 해소, 2026-09-17, minicore-88 세션, commit
5336b50/0d80959] 이 문서 최초의 가장 심각한 발견이었으나 지금은 해결됨**)

- **출처**: `SP-1FBC0EEB` "취소/실패 처리와 Syscall 제안의 연동" 절이
  "`connectChannel` 취소 시 그 `Channel`의 대기열에서 자신의
  `PendingConnectRequest`를 제거"해야 한다고 명시.
- **실제**: `channel.cpp`의 7개 `AsyncTaskHandler` 전부
  `onCancel(AsyncTask*, void*) override {}` - 예외 없이 완전한
  no-op. `ConnectChannelHandler::onExec()`이 코루틴 **로컬 변수**
  `PendingConnectRequest req`를 `channel->pendingConnects`에 매달아
  둔 채 대기하는데, 대기 중 제출자가 죽으면(`PN-40E976F2`가 실제로
  구현한 취소 경로) `onExec()`을 재개하지 않고 `onCancel()`만
  호출한 뒤 그 AsyncTask(코루틴 스택 포함)를 반납한다 - 빈
  `onCancel()`이라 `&req`가 제거되지 않고 **댕글링 포인터**로
  `channel->pendingConnects`에 남는다(다음 `acceptFromChannel`이
  이를 꺼내 역참조하면 UAF). `AcceptFromChannelHandler`도 자기 자신의
  `AsyncTask*`를 `channel->pendingAccepters`에 매달아 두는 구조라
  마찬가지 위험 - 설계 문서의 "acceptFromChannel 취소는 정리
  불필요" 전제가 실제 자료구조와 안 맞는 것으로 보인다.
- **조치**: **PN-C4611402**(completed, commit 5336b50) - 4개 핸들러
  (`ConnectChannelHandler`/`AcceptFromChannelHandler`/
  `ChannelReadHandler`/`ChannelWriteHandler`)의 `onCancel`에 실제
  큐 제거 로직 구현(`AsyncTaskWaitQueue::remove()`/`Channel::
  removePendingConnect()` 신설). `OpenChannelHandler`/
  `CloseBridgeHandler`/`DestroyChannelHandler` 3개는 onExec에
  yield 지점이 없어 no-op 유지가 맞음을 코드로 확인, 근거를 주석으로
  남김. `SP-1FBC0EEB`의 "acceptFromChannel 취소는 정리 불필요"
  서술이 실제로 틀렸음이 확인돼 정정 각주 추가(commit 0d80959) -
  read/write 쪽도 같은 모양의 미서술 갭(`pendingReaders`/
  `pendingWriters`)이 추가로 발견/수정됨.
- **현재 상태(2026-09-17)**: **완전 해소.** QEMU 회귀(무-initrd
  단일코어 + devmgr 포함 SMP4, 실제 Pnp/Channel IPC 정상 경로 포함)
  무회귀 확인. **단, 실제 취소 레이스(대기 중인 제출자를 다른
  스레드가 강제 종료) 자체는 재현 못 함** - 이 코드베이스에 아직
  "임의의 다른 스레드를 강제 종료"시키는 수단이 없어(SelfTerminate는
  자기 자신만, Signal 기반 취소는 PN-71E50394 미연동) 코드 검토로
  대신 검증했다 - `PN-71E50394` 완료 후 재검증 가치 있음(§3에
  후속 항목으로 등록).

### 1-C. `SP-8B6B8D25` §2-B(유저 페이지 폴트 정책) 문서 정체 (문서만 정정 - 코드 갭 아님)

- **출처**: `SP-8B6B8D25` §2-B가 "유저 폴트 시 그 프로세스만
  블로킹시키고, 폴트 정보(주소/에러코드/명령어)를 PCB에 저장해
  나중에 '진짜 오류인지 SWAP 필요인지' 판단"하는 정책을 명시.
- **실제**: `PN-71E50394`(Signal 전달 인프라)가 `QU-04C420BF` 답변
  ("그래 이렇게 해")으로 확정한 최종 정책은 **훨씬 단순**하다 -
  SWAP 자체가 없어(`RM-7C249618` "libswapfs 예정, 미구현") 그
  판단 대상이 없으므로, 온디맨드 매핑/COW로도 못 고친 ring3 폴트는
  `raiseSignal(Segv)` 후 `kTerminateFaultingUserTask()`로 **그
  즉시 종료**(블로킹 후 유예가 아님, 폴트 정보를 PCB에 별도
  보관하지도 않음).
- **조치**: `SP-8B6B8D25` §2-B에 정정 각주 추가(원문 보존) - 이건
  실제로 "설계가 나중에 더 단순한 방향으로 바뀌었는데 원본 문서가
  그 사실을 반영 못 한" 사례로, **코드가 잘못된 게 아니라 문서가
  낡았던 것** - 코드 쪽 조치 불필요.
- **[추가 발견, 2026-09-17]** 같은 낡은 정책 설명이 **두 번째
  문서에도 그대로 복제**돼 있었다 - `SP-68182FBD`(프로세스 모델)
  §2.3이 "폴트 정보를 `pendingSyscalls` 토큰으로 모델링해 비동기로
  나중에 판단"이라는 같은 옛 정책을 자기 언어로 다시 서술해 둔 채였다
  (원본 §2-B 정정과 별개로 놓쳤던 사본). `idt.cpp` 580/645/651행
  재확인(`kTerminateFaultingUserTask`가 즉시 종료, 반환 없음)으로
  같은 정정을 이 문서에도 적용 완료 - "하나를 고쳤다고 다른 문서의
  복제본까지 자동으로 고쳐지지 않는다"는 점을 상기하는 사례로 기록.
- **현재 상태**: 완전 해소(양쪽 문서 모두).

### 1-D. `RM-28225668`(인터럽트 벡터 목록) `0xE2` 항목 - 역방향 문서
정체 (문서만 정정 - 코드 갭 아님, 오히려 코드가 문서보다 앞서 있었음)

- **출처**: `RM-28225668`(신뢰도 높다고 이 감사 문서 §"방법론"이
  스스로 전제한 표 형태 현황판) `0xE2`(`kForcedMigrationVector`)
  항목이 "설계 확정(번호 배정), 구현 대기"로 표시돼 있었음
  (`SP-ECC59BAE` §3.1 대상).
- **실제**: `scheduler.cpp`에 `requestForcedMigration()`/
  `kForcedMigrationIsr`/`ForcedMigrationRequest`가 이미 구현돼
  있고, `idt.cpp`에 `kForcedMigrationVector` 디스패치도 등록돼
  있음을 grep으로 확인 - **실제로는 구현이 완료돼 있었는데 표만
  갱신이 안 된 것**. `RM-32D06563`(용어 문서)는 같은 항목을 "설계
  확정, 구현 계획 등록 완료"로만 적어 뒀을 뿐 완료 여부를 명시하지
  않아 그쪽도 모호했다.
- **의미**: 이 감사 문서의 방법론(§"방법론")은 "표 형태 현황판은
  신뢰도가 높고, 산문형 SP/DC 문서가 위험 지대"라고 전제해 왔다 -
  이번 발견은 표 형태 현황판도 갱신이 밀릴 수 있다는 첫 반례다.
  다만 방향이 반대(코드가 문서보다 앞섬)라 실제 위험(코드 미반영)은
  아니었다.
- **조치**: `RM-28225668` `0xE2` 항목에 정정 각주 추가(원문 보존).
  코드 쪽 조치 불필요.
- **현재 상태**: 완전 해소.

### 1-E. `SP-71DA77B3`(인터럽트 구독) `WaitInterrupt` `onCancel` 미구현 -
댕글링 포인터 위험 (코드 갭, `PN-C4611402`/§1-B와 동일 결함 클래스,
**해소 완료**)

- **출처**: 설계자 지시("설계 공백을 찾아다녀봐")로 재개한 이번 스윕에서
  `SP-71DA77B3`를 처음으로 이 문서 방법론에 대입 - `interrupt_subscription.h`/
  `.cpp`(PN-B3DD3D19가 구현)와 대조.
- **문제**: `WaitInterruptHandler::onExec`이 이벤트가 없으면 자기 자신
  (`AsyncTask*`)을 `slot->waiters`(`InterruptWaiterQueue`, channel.h의
  `AsyncTaskWaitQueue`와 동일한 침습적 FIFO)에 매달아 두고 파킹하는데,
  `onCancel(AsyncTask*, void*) override {}`가 **완전한 no-op**이다 -
  제출 UserThread가 대기 중 죽으면(`PN-40E976F2`의 취소 경로) 그
  포인터가 제거되지 않고 댕글링으로 남아, 다음 인터럽트의 ISR이
  `waiters.popFront()`로 그걸 꺼내 `AsyncReactor::submitCompletion()`을
  호출하는 순간 UAF가 된다. **§1-B(Channel IPC)에서 이미 발견·수정한
  것과 정확히 같은 패턴**(코루틴 자신의 `AsyncTask*`를 침습적 대기
  큐에 걸어 둔 채 파킹 → onCancel이 그 큐에서 제거 안 함) - `PN-B3DD3D19`
  (2026-09-15 착수)가 §1-B의 수정(`PN-C4611402`, 2026-09-17)보다
  먼저 만들어져 그 교훈이 반영되지 못한 것으로 보인다.
- **조치**: **`PN-BD276A24`**(scheduled) 등록 - `InterruptWaiterQueue`에
  `AsyncTaskWaitQueue::remove()`와 동일한 패턴의 `remove()` 추가,
  `WaitInterruptHandler::onCancel`에서 `args->vector`로 슬롯을 찾아
  호출. `SubscribeInterrupt`/`UnsubscribeInterrupt`/`GetInterruptDump`
  세 핸들러는 onExec에 yield 지점이 없어 no-op 유지가 맞음(§1-B와
  동일 논리). `SP-71DA77B3`에 정정 각주 추가 완료.
- **의미**: 이 문서(§5 기록 규칙)가 "한 문서를 고칠 때 같은 설명이
  복제됐는지 의심하라"고 이미 적어 뒀는데, 이번 발견은 그보다 한
  단계 더 일반적인 패턴을 보여준다 - **한 서브시스템에서 잡은 결함
  클래스(onCancel 미구현)가 비슷한 시기에 독립적으로 설계/구현된
  다른 서브시스템에도 그대로 재현될 수 있다**. 앞으로 이런 "구조적
  결함 클래스"를 하나 잡을 때마다, 같은 침습적 대기 큐 패턴을 쓰는
  다른 서브시스템(예: `WaitInterruptHandler`와 구조가 같은 향후
  핸들러)이 있는지도 함께 훑는 것을 이 문서의 표준 절차에 추가할
  가치가 있다.
- **[완료, 2026-09-17, commit 58c7416]** `InterruptWaiterQueue::remove()`
  추가 + `WaitInterruptHandler::onCancel` 구현 완료 - 단 `args->vector`로
  owner를 재조회해 슬롯 하나만 찾는 대신(취소 시점엔 `submitterTask`가
  이미 비어 있을 수 있어 `ConnectChannelHandler`류가 그 경로를 피한 것과
  같은 이유), 그 벡터의 구독자 슬롯(최대 8개) 전부를 훑어 제거하는
  더 단순한 방식으로 구현(`PN-BD276A24` 참고). `Subscribe`/`Unsubscribe`/
  `GetInterruptDump`는 no-op 유지(이유 주석 추가). `InterruptWaiterQueue::
  remove()` 자체는 TEMP 단위 테스트(head/중간/tail/미존재)로 검증,
  실제 취소 레이스 왕복은 `PN-C4611402`와 동일하게 코드 감사로 검증
  (재현 수단 자체가 `PN-B5C2845A` 대기 중이라 동일한 제약).
- **현재 상태**: **완전 해소.**

### 1-J. `SP-83A07867`(CR3 동기화 통합) §3.2 - Task-to-Task 직접 전환이
"CR3 재동기화 필요 지점 정확히 세 곳" 목록에서 빠짐 (코드 갭,
완전 해소, 2026-09-18, commit `fb06753`)

- **출처**: minicore-88이 `PN-87D6B615` "남은 범위 2번" E2E 재현 중
  실측 발견(`PN-B5FD7B75`) - `SP-83A07867` §3.2가 "CR3 재동기화가
  필요한 재개 지점은 정확히 `kTaskStartTrampoline`/`yieldCurrent`
  재개/`parkCurrent` 재개 이 세 곳뿐"이라고 확정해 둔 전제가 실측으로
  깨졌다.
- **문제**: `Scheduler::onTick()`의 Task-to-Task 직접 전환
  (`kContextSwitch(&current->savedRsp, next->savedRsp)`)이
  `current->savedRsp`를 `onTick()` 함수 본문 한가운데(그
  `kContextSwitch` 호출 바로 다음 줄)에 남기는데, 이 지점이 그
  "정확히 세 곳" 목록에 없다. 나중에 이 `current`가 `runLoop()`의
  idle→Task 디스패치 경로(§3.2가 "도착 지점이 항상 그 세 곳 중
  하나이므로 CR3 재동기화 불필요"라고 명시한 바로 그 최적화)로
  재선택되면 CR3가 전혀 재동기화되지 않아, 재개된 Task가 자기 코드를
  실행하는 순간 즉시 #PF로 죽는다.
- **재현**: 단일 코어(SMP=1)에서 devmgr이 자신의 자식(dbgtarget)을
  DebugContinue로 같은 코어에서 즉시 Ready시킨 뒤, devmgr 자신의
  `submit()`/`wait()` 사이 짧은 구간에 스케줄러 틱이 끼어드는 정확한
  타이밍 경쟁 - 실측 재현율 ~3회 중 1-2회. `ring3 #PF task=devmgr
  rip=cr2=0x400052 cr3=0x106000`(devmgr 고유 pml4Phys는 0x1633000)
  으로 CR3-RIP 불일치가 시나리오와 정확히 일치함을 로그로 확인. 이전
  "코어 간 마이그레이션 가설"은 이 실측으로 기각됨(단일 코어에서도
  재현) - 진짜 원인은 같은 코어 안에서의 preemption 타이밍 경쟁.
- **왜 지금까지 안 드러났는가**: 이 버그는 "한 코어에 동시에 Ready인
  서로 다른 두 Task"가 있어야 발현되는데, 기존 스케줄러 테스트는
  대부분 멀티코어라 두 번째 Task가 대개 Push/Pull로 다른 코어로
  가버려 `onTick()`의 Task-to-Task 분기 자체가 잘 안 트리거됐다.
  단일 코어 + 같은 코어에서 즉시 Ready(디버그 세션)라는 이 세션
  전체에서 처음 만들어진 조합이라 처음 드러남.
- **조치**: `QU-29793535`(설계자 답변 대기, `PN-B5FD7B75`에 세 후보
  정리) - (A) `onTick()`의 그 `kContextSwitch` 다음 줄에
  `kSyncCr3(current)` 추가(권장 - §3.2의 "트랩 진입점에서 동기화"
  철학과 대칭), (B) `runLoop()` idle 디스패치가 항상
  `kSyncCr3(next)` 호출(§3.2 최적화 포기, 안전하지만 불필요한 호출
  추가), (C) 둘 다(과설계 가능성). 스케줄러 핫패스라 이 세션은 직접
  QEMU 검증 없이 발행하지 않는다는 원칙대로 구현 전 확인을 구한
  상태 - 아직 미수정.
- **[답변, 2026-09-18]** `QU-29793535` 설계자 답변: "스위칭이 일어나는
  순간에 CR3를 바꾸는게 맞다고 생각이 드는데." - (A)안(그 `kContextSwitch`
  다음 줄에 `kSyncCr3(current)` 추가) 채택으로 확인됨. minicore-88이
  질의 소유자로 직접 확인·resolved 처리(2026-09-18 05:39) - 이
  세션의 relay 불필요.
- **[완전 해소, 2026-09-18, commit `fb06753`]** `Scheduler::onTick()`의
  그 `kContextSwitch` 다음 줄에 `kSyncCr3(current)` 추가 - 정확히
  방향 A 그대로 구현됨. **부수 발견**: `Scheduler::onForcedMigration()`
  도 동일한 `kContextSwitch(&current->savedRsp, next->savedRsp)`
  Task-to-Task 직접 전환 패턴을 갖고 있어 같은 네 번째 재개 지점
  문제에 노출돼 있음을 구현 세션이 스스로 찾아내 같은 수정을 함께
  적용함(원 버그 리포트/설계자 답변엔 `onTick()`만 언급됐으나 같은
  결함 클래스를 능동적으로 확장 점검한 사례). devmgr+dbgtarget E2E
  하네스로 SMP1 + 동시 Ready 유저 태스크 2개 조합 재현 - 수정 전
  100% 재현(4번째 Task-to-Task 전환에서 cr3=gBootPml4Phys로 #PF),
  수정 후 8회 반복 무크래시(그중 6회는 이 취약 경로가 실제로 실행됨을
  breadcrumb으로 확인). `kSyncCr3`의 skip-if-same 최적화로 흔한
  재개 경로(CR3가 이미 맞는 경우)엔 추가 비용 없음.
- **현재 상태**: **완전 해소.**

### 1-K. `PageFrame` 구조체 - 구조체 교체 + rmap/LRU 1단계 배선 완료 (완전 해소, 잔여는 미래 트리거로 분리)

- **출처**: `SP-6CEFBE9B`("물리 페이지 프레임 메타데이터 — PageFrame
  구조체", 2026-09-18 approved)가 rmap(§6)/swap LRU(§7)/캐시타입
  일관성(§3) 필드를 포함한 64바이트 `PageFrame` 구조체를 확정했다.
- **[완료, 2026-09-18, commit `bd43196`, PN-2FC5ED36 항목1-2]**
  `page_frame_allocator.h`/`.cpp`가 기존 `uint16_t` 배열을 실제
  `PageFrame[]`(64바이트, `static_assert` 확인)로 교체 완료 - 코드를
  직접 읽어 설계와 대조한 결과 필드/플래그/주석 전부 `SP-6CEFBE9B`
  §1/§2/§5와 정확히 일치함을 확인(설계 이탈 없음). `retain()`/
  `refCount()` API 시그니처 불변, `PG_RESERVED`/`numaNode` 실제
  세팅, 신규 `frameFor(physAddr)` 접근자까지 계획대로 구현됨. QEMU
  3개 표준 시나리오 무회귀 실측 확인.
- **[완료, 2026-09-18, commit `dea9f1c`, PN-2FC5ED36 항목3-4]**
  `insertRmap()`/`removeRmap()`(page_frame_allocator.cpp)을 코드로
  직접 확인 - `SP-6CEFBE9B` §6.2(삽입/제거 규칙)·§7.2 1단계(최초
  진입 시 inactive 리스트 push, 중복 삽입 방지)와 정확히 일치.
  `ProcessAddressSpaceManager`의 `mapRegion`/`registerFixedRegion`/
  `resizeAnonymousRegion`(Anonymous 매핑 지점 3곳 전부) → `insertRmap`,
  `kRollbackMapped`/`unmapRegion`/`unmapAll` → `removeRmap` 배선
  확인. `freeOrder()`에 설계 문서엔 없던 방어적 rmap/LRU 청소까지
  구현 세션이 스스로 추가(정상 경로의 안전망, 설계 이탈이 아니라
  타당한 보강). devmgr+fs 2-프로세스 실측(15+12회) 무크래시 확인.
- **의도적으로 남은 잔여 항목(둘 다 이 문서의 "갭"이 아니라 아직
  실사용처가 없는 것으로 판단 - 각자 명확한 미래 트리거가 있어
  `PN-2FC5ED36`은 completed로 종결하고 아래로 분리 추적)**:
  1. `kHandleCowWriteFault`의 rmap "이동"(COW 새 프레임으로 엔트리
     이전) - `fork()`(`PN-44C91D6E`) 착수 전까지는 COW 자체가 트리거될
     길이 없어 검증 불가능이라 미배선(합리적 판단) - `PN-44C91D6E`
     착수 시 함께 배선.
  2. LRU §7.2 2단계(`PG_ACCESSED` 세팅) - `paging.cpp` 전수 확인
     결과 참조 0건(미배선 맞음) - v1은 재폴트 경로 자체가 거의 없어
     소비처가 없다는 판단이 타당함(`SP-6CEFBE9B` §7.3 기존 합의와
     일치) - swap 착수 시(`PN-4859FDE9`) 재검토.
  3. `elf::loadIntoAddressSpace()`의 PT_LOAD 세그먼트가 `VmaBacking::
     Anonymous`로 등록되는지는 `elf.cpp`가 이 저장소 `minicore/kernel`
     밖에 있어 미확인(`PN-2FC5ED36`에 참고용으로 남김, Anonymous가
     아니면 rmap 커버리지에 조용한 공백 가능성 - 다음에 ELF 로딩
     경로를 손댈 세션이 확인 권장).
- **부수 확인**: 같은 날 `PN-9E2CC631`(FileBacked 캐시 정책)도
  완료돼 `SP-6CEFBE9B` §6.3/§7.4가 "Anonymous/FileBacked 공유 LRU"로
  갱신됐다(정책 확정, 코드는 fs 서비스 실코드 대기).
- **현재 상태**: **완전 해소** - 설계가 요구한 구조체/rmap/LRU 1단계
  전부 코드로 구현·검증됨. 캐시타입 불일치 처리는 `PN-81223433`, swap
  스캔 트리거 정책은 `PN-4859FDE9`, COW rmap 이동은 `PN-44C91D6E`
  착수 시로 계속 별도 추적.

## §2. 점검 완료 - 갭 없음 확인

- **`PN-C4611402`(Channel IPC `onCancel`, "connectChannel 취소:
  대기열에서 자신의 PendingConnectRequest 제거")** - [2026-09-18]
  당시 코드 검토로만 확인했던 "댕글링 포인터 없음"을 `PN-B5C2845A`가
  열어 준 실제 Kill-중-파킹 레이스로 QEMU에서 처음 재현/확정했다.
  devmgr이 자기 채널에 `connectChannel`로 무기한 파킹 → 외부에서
  `raiseSignal(Kill)` → `ConnectChannelHandler::onCancel()` →
  `Channel::removePendingConnect()`가 실제로 그 대기 항목을 큐에서
  제거해 `pendingHead`/`pendingTail`을 정확히 비움을 3회 반복 확인
  (TEMP 스캐폴딩, 원복 완료 - 발행할 프로덕션 diff 없음). 갭 없음 -
  설계/코드/실측 3단이 전부 일치.

- **`SP-D7013B26`(Slab 할당자/libkmm)**: "확정된 최종 설계" 절이 나열한
  전 항목(더블 매거진 loaded/previous, `PreemptionGuard` 강제, 매거진
  용량 16 고정, 버킷 7단계 32/64/128/256/512/1024/2048, `sizeToBucket()`
  단일 경유, 슬랩 Order 0 고정, 고갈 시 즉시 nullptr 비블로킹, 2048B
  초과는 PageFrameAllocator 직행)를 `libkmm/slab.h`/`slab.cpp`와 한
  줄씩 대조 - 예외 없이 전부 설계 그대로 구현돼 있음을 확인
  (`Scheduler::disablePreemption`/`enablePreemption`도 scheduler.h에
  실재). 갭 없음 - 이 프로젝트에서 보기 드물게 처음부터 끝까지 정확히
  구현된 사례로 기록.

- **`TaskClass::RealTime` 우선 스케줄링**: `task.h` 주석이 "구현
  예정"이라고 남아 있어 의심했으나, `Scheduler::pickNext()`가 실제로
  Immediate→RT→Normal 순서로 큐를 비우는 것을 `scheduler.cpp`에서
  확인 - 정상 구현됨(코드 주석만 안 지워진 사소한 흔적, 별도 조치
  불필요).
- **`SP-68182FBD`(Process/AddressSpace 설계)**: 열린 항목마다
  "해결됨" 표시와 참조 문서가 일치 - 갭 없음.
- **`RM-48E1E610`(Syscall 할당표)**: 두 세션이 매 틱 갱신하며 실측과
  맞춰 옴 - 표 형태라 신뢰도 높음. 이번 조사에서 불일치 없음.
- **`RM-B5764185`(Signal 번호표)**: v1에서 실제로 발생 가능한 4개
  신호(Kill/Term/Segv/Ill)와 `PN-71E50394` 구현이 일치 - 갭 없음.
- **`RM-C65F7760`(VFS 구조)**: "구현된 경로" 표가 이미 "아직 실측
  검증 안 됨" 항목(kernel/<name> 인증 성공 분기)을 스스로 정확히
  인지하고 있음 - 숨겨진 갭 아니라 이미 추적 중인 미검증 항목.
- **`RM-28225668`(인터럽트 벡터 목록)**: 0xE0-0xE3 전부 상태 최신,
  `PN-B3DD3D19`(인터럽트 구독)가 새 고정 IPI 벡터를 요구하지 않고
  기존 동적 벡터 위임 메커니즘을 재사용함을 확인 - 갭 없음.
- **`PN-18FDBFF3`(Channel ownerProcess → DontDeref&lt;Process&gt; 승격)**:
  minicore-88 완료 보고(commit f454faf) - `shared_ptr.h:538`의
  `DontDeref<T>`(534행 주석에 "operator*/operator->/T* 변환 전혀
  없음" 명시, g++ -fsyntax-only로 operator-> 실제 컴파일 에러까지
  확인했다고 보고) + `channel.h:293/324`의 타입 교체 전부 코드로
  직접 확인. `RM-32D06563`에도 `DontDeref<T>`/세대 태그 슬롯 테이블
  공용 패턴 둘 다 신규 등록됨(RM-F2DAFF66 방법론이 요구하는 "새
  개념은 RM-32D06563에" 규칙 8/12를 스스로 챙긴 사례).
- **[좋은 사전 포착 사례]** minicore-88이 `PN-E82744B1`(Mutex/
  Semaphore syscall) 착수 전 재검토 중 `SP-0666DB3C` §17.2가
  Channel과 똑같이 "핸들=포인터값" 관례를 그대로 물려받고 있었음을
  스스로 발견 - `PN-CE6A04AB`로 그 관례 자체가 보안 취약점이었던
  걸 이미 아는 상태였기에, 구현 착수 전에 §17.2 정정 각주 + 착수
  조건에 세대 태그 테이블 패턴 필수화를 미리 걸어 뒀다(§17.2/
  `PN-E82744B1` 코드 대조로 확인). RM-F2DAFF66이 추적하는 "한 문서의
  낡은 설명이 다른 문서에도 복제돼 있을 수 있다"(§5 방법론)는 것과
  정확히 같은 패턴을 이 세션 밖에서도 스스로 잡아낸 사례 - 별도
  조치 불필요, 기록만.
- **`PN-CE6A04AB`/`SP-CA3C3E57`(Channel 보안 취약점)**: minicore-88이
  구현 완료(commit 74f0f75) 보고, 이번 틱에 코드 독립 확인 -
  `channel.h:293`(`ownerProcess`), `channel.cpp`의 `kResolveChannelId`
  (:166)/세 호출부 교체(:403/:472/:501/:612/:835)/소유자 검증
  (:510-512, :843-845) 전부 실측 확인, `gChannelTable[65536]` 크기도
  §2 확정값과 일치. RM-F2DAFF66이 추적하던 항목 중 실제로 완전히
  닫힌 사례(PN-C4611402/NUMA와 같은 급) - 잔여 항목은 §6-A
  `DontDeref<T>` 타입 승격(`PN-18FDBFF3`)/SharedPtr 마이그레이션
  (`PN-260D7D73`) 둘 다 별도 계획으로 openly 추적 중이라 갭 아님.
- **`SP-9CB55C5B`(Kill 안전한 ProcessId 해석)**: `kResolveProcessId()`/
  `gProcessTable[]` 자체는 아직 코드에 없음(`process.h` grep 0건) -
  다만 이건 문서 §7이 스스로 "제안(확정 아님)"이라고 명시한 것과
  일치하고, `PN-88E62419`(Kill 임의 대상 구현, in_review)가
  `PN-C39882D0`/`PN-AA30E4C8`/`PN-617F4E52` 세 선행 계획으로 이미
  openly 추적 중이라 "조용히 빠진" 사례는 아니다(§4 예방조치 패턴과
  동일). **부수 발견**: `PN-C39882D0`(pid ABI 마이그레이션 승인)의
  자체 선행 조건("SP-9CB55C5B approved + QU-78E4159E 해소")이 이미
  충족돼 있었는데도 `planned` 상태로 방치돼 있었음 - 착수 전 확인
  사항(유저랜드 pid 소비자 존재 여부, `docs git grep` 결과 0건)을
  이번 틱에 완료하고 `QU-AB5247DD`로 명시적 승인 요청 등록,
  `pending_approval`로 전환(이 발견 자체는 코드 갭이 아니라 계획
  진행 누락이라 이 문서보다 일반 루프 절차 2번에 해당하지만, "설계는
  확정됐는데 후속 조치가 멈춰 있었다"는 성격이 같아 여기 기록).
- **`SP-E9B44929`(syscall 그룹+call 2단계 인코딩)**: minicore-88
  완료 보고(commit 1f9228d) 독립 검증 - `syscall.h`의
  `kMakeSyscallEndpointId`/`kSyscallGroupOf`/`kSyscallCallOf` +
  `kSyscallEndpointSelfTerminate = kMakeSyscallEndpointId(0, 0)`류
  재정의 확인, `syscall.cpp`의 `gCallSlotPool[1024]`(정적 범프 풀 -
  최초엔 `GenericSlabAllocator`로 동적 할당했다가 `SelfTerminateHandler`
  등록이 `GenericSlabAllocator::init()`보다 먼저 실행되는 부팅
  순서 때문에 페이지 폴트 패닉 - 실측으로 스스로 잡고 정적 풀로
  교체) 확인. `RM-48E1E610`도 그룹별 챕터로 실제 재구성됨(그룹 0
  Process부터 확인) - Sync 그룹(8)은 §17.2 정정 대기로 의도적으로
  번호만 예약 상태 유지, 경고 문구까지 남아 있음. 완전히 닫힌
  사례 - 갭 없음.
- **`SP-9F1DB1D8`(gCurrentTask RwSpinlock)**: `PN-D3597800`
  completed(commit 2d0da74) 주장 독립 검증 - `spinlock.h:129`에
  `RwSpinlock`/`RwSpinlockReadGuard`/`RwSpinlockWriteGuard` 확인,
  `scheduler.cpp`에 `gCurrentTaskLock[kMaxCores]` + 18곳의 가드
  적용(읽기/쓰기 전부) 실측 확인 - 구현 중 추가로 발견됐다는
  `retireCurrentTask()`/`handleFpuTrap()` 두 곳도 실제로 가드가
  걸려 있음. 갭 없음.
- **`SP-1DB13F61`(vtable 타입 placement new 예외)**: `shared_ptr.h`에
  `kMakeSharedNew<T>()`/`kDestroyCtorAndFree<T>()`(§3 제안 그대로)가
  실제로 구현돼 있고, `mutex_core.h`/`semaphore_core.h` 둘 다 예전
  raw `GenericSlabAllocator::alloc`+`memset` 패턴이 남아있지 않음을
  확인(주석이 `kMakeSharedNew<Mutex>()`/`kMakeSharedNew<Semaphore>()`
  로 만들어야 한다고 명시) - 갭 없음.
- **`SP-00CA7175`(커널 ↔ 커널 서비스 통신 채널)**: 문서 자체가 이미
  2026-09-17에 "전면 정정"/"전부 완료" 각주를 달아 두었으나(RM-F2DAFF66
  방법론 - 문서의 자체 완료 선언은 그 자체를 의심하고 재검증), 이번
  세션이 독립적으로 코드 대조: `channel.h`(281/303행)의
  `Channel::exclusivePreemptive` 필드 + `channel.cpp`(10곳)의
  `AsyncReactor::submitCompletion(..., channel->exclusivePreemptive)`
  전달로 Tier B 완전 구현 확인, `kernel_service_ring.h`(16행)에
  `KernelServiceSharedRingBuffer` 구조체(Tier A 골격) 존재 확인 -
  `PN-7AC01E6E`(completed)가 자체 기록한 "항목 7: 구조체만, 실제
  소비자 없음"과 정확히 일치. **결론**: 문서의 자체 정정이 실제로
  정확했다 - 갭 없음(남은 유일한 열린 항목인 Tier A 소비 알림
  메커니즘/버퍼 크기는 실제 소비자가 생기기 전까지 정당하게 유예된
  상태, RM-23F4B687 §4 패턴).
- **`SP-EAB162FC`(ProcessRole/Capability/Resurrect 체계)**:
  §2.1(`Process::role` 필드)/§2.3(SubscribeInterrupt exclusive
  자격 검증)/§6.1-6.4(Resurrect `essential`/`resurrect` 플래그,
  3단계 분기, 백오프 상수·공식까지 정확히 일치) 전부 `process.h`/
  `scheduler.cpp` 실제 코드와 대조 확인 - 갭 없음. §2.2(PnP 드라이버
  자식도 KernelService 부여)/§2.3의 RequestIoPermission 소비는
  그 상위 기능(PnP 드라이버 스폰, RequestIoPermission 자체)이 아직
  코드로 없어 지금은 대조 불가 - devmgr 항목4/5 착수 후 재확인 필요
  (§3에 다시 추가하지 않고 여기 각주로만 남김, 그때 가서 다시 봄).
- **`SP-2AAD7C8D`(mmap/Maple Tree) §6**: 6개 항목 중 2(TLB
  샷다운)/4(파일 백킹 mmap)/6(COW)은 명시적 ~~취소선~~/"완료"
  표시로 해소 확인됐고, 1(RCU)/3(findGap 시작 지점)/5(노드 전환
  휴리스틱)은 전부 "구현 시점에 실측하며 정한다"고 처음부터 명시적
  으로 유보된 항목(RM-23F4B687 §4 정당한 유예, 숨겨진 갭 아님) -
  전체적으로 갭 없음.
- **`SP-8B6B8D25` §2 항목9 "CPU 캐시 관리"**: 프로젝트 전체 문서
  검색(`MTRR`/`wbinvd`)에서 0건 - 이 마스터 문서가 커널 책임으로
  이름만 올려 뒀을 뿐 구체적인 하위 SP 문서 자체가 한 번도 작성된
  적이 없다(지금까지의 §1/§2 항목들과 성격이 다름 - "확정된 설계가
  코드에 빠진 것"이 아니라 "구체 설계 자체가 없는 것"). 가장 실질적인
  우려였던 MMIO 캐시 일관성은 `pnp.cpp`의 `RequestIoPermission`
  구현이 `PAGE_CACHE_DISABLE`을 이미 올바르게 설정하고 있어 실재
  버그는 아님을 확인 - **`PN-5BCA7AB9`로 낮은 우선순위 문서화
  백로그 등록, 갭 없음(급한 위험 아님)**.
- **`SP-39F18E30`(DMA 버퍼 관리자)**: `AllocDmaBuffer`/`FreeDmaBuffer`
  syscall 자체가 아직 미구현 - `RM-48E1E610`("번호만 예약")과
  `pnp.cpp` grep(구현 없음) 둘 다 일치, 표 형태 현황판이 이번엔
  정확했다. §6이 스스로 열어 뒀던 3개 항목 중 2개는 이미 해소
  기록, 나머지 하나(프로세스 종료 시 물리 프레임 반납 누수)도
  `PN-FFC2F062`로 착수 조건(DMA 버퍼 관리자 자체 착수)까지 명시해
  정확히 추적 중 - 실제 드라이버(AHCI/USB)가 아직 하나도 없어
  당장 필요하지도 않다(PN-BD9AAE2F 항목5와 같은 이유). 갭 없음.
- **`SP-29D652AA`(진짜 컴파일러 thread_local)**: §7까지 전부 확정된
  approved 설계이지만 실제 구현은 `PN-22E5E9E7`(scheduled, 미착수)
  으로 이미 정확히 등록돼 있음을 `plan_get`으로 확인 - 디스패치
  핫패스를 건드리는 위험도 때문에 의도적으로 미착수 상태(minicore-88
  세션이 이미 인지하고 보류 중) - openly 추적 중, 숨은 갭 아님.
  **[갱신, 2026-09-17] 툴체인 자체가 원인이었던 것으로 최종 확정 -
  x86_64-elf-gcc 크로스컴파일러로 전면 교체 완료(commit e7943e0)**:
  이 문서가 여러 틱에 걸쳐 추적해 온 `PN-22E5E9E7`의 lld PT_TLS
  결함 조사(QU-58D13EAE~QU-90A616DA 5라운드, lld 18/19/20 전수 기각→
  gcc 혼입/오브젝트 신선도/PT_LOAD 구조/higher-half 전환 전부 배제→
  "실제 오브젝트 세트 조합에 의존하는 lld 다중 오브젝트 TLS 크기
  합산 결함"으로 좁혀짐)가 설계자 직접 지시로 해결됐다 - 호스트
  배포판 clang+lld 대신 `/opt/cross`의 `x86_64-elf-gcc 13.2.0 + GNU
  Binutils 2.42`(이 타깃 전용 진짜 크로스컴파일 툴체인)로 전면 교체,
  같은 오브젝트 세트로 `PT_TLS.p_memsz`가 정확히 계산됨을 실측
  확인. `DS-D4E5C451`(핵심 결정 문서, "컴파일러: WSL clang")에 정정
  각주가 이미 정확히 반영돼 있음을 독립 확인(원문 보존, 교체 사실+
  근거+영향받은 플래그 3종 명시). 코드젠 차이로 `-fcoroutines`/
  `-fno-threadsafe-statics` 플래그 추가, `.init_array` 출력 섹션
  신설+`kmain.cpp`의 `kRunGlobalConstructors()`(BSP 극초반 1회 순회)
  배선까지 확인 - GCC가 clang과 달리 일부 전역 객체를 진짜 동적
  초기화로 코드젠한다는 사실이 새로 드러난 것으로, 앞으로 이
  프로젝트의 "정적 초기화만으로 충분하다"는 암묵적 전제를 쓰는 전역
  객체가 있다면 재검토 가치 있음(당장 코드 갭은 아님 - 일반 방어
  메커니즘이 이미 배선됨). `PN-22E5E9E7` 자신의 원래 구현 범위(항목
  1-7, TLS 배선)는 이 블로커 해소로 이제 착수 가능하나 아직 미착수
  (계획 status 여전히 scheduled, 2026-09-17 기준) - 숨은 갭 아님,
  다음 착수 시 재대조 필요.
- **`SP-C2670F69`(AHCI)**: §4 항목2("fs 서비스 설계가 아직 없음")가
  낡은 교차 참조였음을 발견 - `SP-7CC5693A`(fs/VFS)가 그 사이
  approved되며 §3.2 `FileSystemDriver::mount(BlockDevice*)`가 정확히
  `AhciBlockDevice`를 연결점으로 지목해 뒀는데 이 문서는 갱신이 안
  돼 있었다. 정정 각주 추가 - `BlockDevice` 인터페이스 세부(LBA
  read/write/TRIM 등 정확한 시그니처)는 여전히 미정이지만 코드가
  전혀 없는 순수 설계 단계 세부라 별도 PN 등록은 보류(AHCI 실착수
  시 자연히 확정될 항목, RM-23F4B687 §4 취지) - 갭 없음(정정만).
- **`SP-E35FD36C`(USB 스택)**: xHCI 위주 v1 설계 + 레거시(UHCI/OHCI/
  EHCI)/SuperSpeed 확장 초안까지 전부 명시적으로 유예 조건과 함께
  기록돼 있고, 코드가 전혀 없어(devmgr 자식 프로세스로 실행 예정,
  아직 착수 전) 코드-문서 불일치 자체가 성립하지 않는다 - 교차
  참조(SP-9DD4F3EA/SP-39F18E30)도 최신 상태와 일치. 갭 없음.
- **`SP-83A07867`(CR3 동기화 통합)**: §3.2/§8이 "모든 디스패치
  재개 지점"을 두 갈래로 완결했다고 서술하지만, 작성 시점 이후
  생긴 세 번째 재개 경로(`AsyncReactor::drainOnce()`의
  `coroHandle.resume()`, 코루틴 지원과 함께 도입됨)가 빠져 있음을
  발견 - 이미 `PN-2008220B`로 별도 추적 중인 바로 그 갭이라 새로운
  코드 갭은 아니지만("작성 시점엔 정확했던 문서가 이후 생긴 새
  경로를 못 따라간" 사례), 이 "완결됐다"고 주장하는 문서 자체에
  교차 참조가 없어 다음에 §3.2/§8을 참고하는 사람이 오도될 수
  있었다 - 교차 참조 추가(원문 미수정). §8의 하드웨어 불변조건
  체크리스트(CR3/RSP0/FPU) 자체는 명시된 세 지점 안에서는 정확히
  구현/완료돼 있음을 확인(PN-40210D5A/PN-AEA74E1B/PN-F258698E 전부
  completed) - 문서 정정만, 코드 갭 아님.
- **`SP-DE19BB1C`(커널 영역 TLB 샷다운)**: 이 문서 자체가 이미
  `PN-D132A1E9`(§5-1, 유저 영역 확장 - 요청자별 슬롯+수신자별
  Target Pending Mask) 완료를 상세히 기록해 뒀고, `tlb_shootdown.cpp`
  grep으로 `gRequests[kAcpiMaxCpus]`/`gPendingMask[kAcpiMaxCpus]`
  구조가 설계 그대로임을 재확인 - `RM-28225668`의 `0xE0` 등재도
  일치. §5-2(벡터 배정)/§5-3(최적화 기법)도 전부 명시적으로 해소/
  유예된 상태 - 갭 없음.
- **`SP-2602CAA6`(커널 이벤트 발행/구독)** §4/§6/§7/§8(커널 전용
  부분, "즉시 착수 가능"으로 명시된 범위): `event_topic.cpp`를 직접
  grep/read해 `EventTopicRegistry::registerTopic/subscribeKernel`,
  `EventPublisher::publish` 전부 스펙 그대로 구현돼 있음을 확인
  (`PN-AAE631EA`가 커버, TEMP 실측 검증까지 완료 기록). §5(유저
  노출 syscall 3종)는 §13이 명시적으로 "devmgr 실사용처 등장까지
  보류"한 것 그대로 미구현 - openly 추적 중이라 갭 아님. (참고:
  `document_get`의 `backlinks`는 문서↔문서 링크만 보여주고 plan의
  `refs`는 안 잡혀서 처음엔 "계획이 전혀 없다"고 오판할 뻔함 -
  `plan_get`으로 직접 확인해야 정확함, 다음 스윕에서 같은 실수
  주의.)
- **`SP-9DD4F3EA`(PnP 프레임워크) §3 전체**: devmgr 실코드
  (`PN-BD9AAE2F`)가 아직 착수 전이라 "코드에 이미 빠진 것"은 없음
  (§3.1-§3.4 전부 그 계획의 6단계 체크리스트로 이미 openly 추적
  중). 다만 **§3.3a("자원 소유권 및 정리")가 "공식 요구사항"으로
  명시한 `DeviceOwnerTable` ↔ `PN-71C3D483`(Process Teardown Hook)
  연동이 `PN-BD9AAE2F`의 체크리스트 항목4 문구엔 명시적으로 안
  드러나 있어 착수 시 누락될 위험**을 발견 - 착수 전에 예방적으로
  `PN-BD9AAE2F` 항목4에 그 요구사항을 명시적 하위 항목으로
  추가해뒀다(`SP-39F18E30`§3.2의 `Process::dmaBuffers` 정리 경로가
  이미 같은 패턴의 선례). "아직 안 생긴 코드가 빠짐없이 요구사항을
  반영하도록 체크리스트를 보강"한 사례 - §1(이미 발생한 갭)은 아니고
  §4(예방 조치)로 분류.
- **`SP-0666DB3C`(Mutex/Semaphore/Signal) §17/§9.6-3**: §17(유저
  syscall 노출, `MutexCreate` 등 8개는 `RM-48E1E610`에 번호만
  예약)과 §9.6-3 소비 쪽(`Task::lastCancelReason`을 실제로 읽는
  대기 프리미티브가 아직 없음)은 **"숨겨진 갭"이 아니다** -
  `PN-71E50394`/`RM-48E1E610` 자신이 이미 "아직 미구현/번호만 예약"
  이라고 공개적으로 추적 중인 항목이다. 이 문서(설계공백 검수)의
  대상은 "아무도 모르게 빠진 것"이지 "알고 있고 계획된 미착수 작업"
  이 아니므로 구분 - 이런 항목은 그냥 기존 PN 추적에 맡기고 이
  문서에는 안 옮긴다(점검은 했으나 §1 대상 아님, §2에도 안 옮기고
  여기 기록만 남김).
- **`SP-F682B889`(AsyncTask 프레임워크) §3/§7/§8/§9**: 이 문서는 이미
  스스로 대량의 "확정/정정" 각주를 누적해 온 문서라(§3.4가 리액터를
  전용 Task→인라인 idle 경로로, §7.3이 재개 드라이버를
  reactorTaskEntry→drainOnce로 뒤집는 등) 특히 위험 지대일 수 있다고
  보고 실제 `async_task.cpp`와 대조 - **최신 확정 구조
  (homeCoreIndex/allowCoreMigration, coroHandle을 drainOnce()가
  resume, 별도 reactorTaskEntry 없음)가 코드에 정확히 반영돼 있음을
  grep으로 확인**(2026-09-17). §8.6/§9.5의 "아직 열려 있는 하위
  과제"들은 전부 PL-1E247831/PN-C62F7908 구현 시점으로 명시적으로
  미뤄진 항목(RM-23F4B687 §4 정당한 유예 패턴)이라 숨은 갭 아님 -
  갭 없음.
- **`SP-677210E6`(TSS/IST) "이번 범위에 포함하지 않은 것"**: 4개
  항목(RSP0/IST5-7/NMI·MC·DB 실처리/#DF 레지스터 복구) 전부 각자
  PN 계획(PN-124C105B completed, PN-5377545F, PN-F443FE73 completed,
  PN-AD3B2D5B)으로 명시적으로 openly 추적 중 - 숨은 갭 아니다.
  PN-F443FE73을 실제로 열어 확인한 결과 NMI/#MC/#DB 커널 라우팅과
  QEMU 실측까지 전부 완료, 그 안에서 파생된 "#DB 유저 syscall
  범위" 후속 질의(QU-3DB5F85C)도 빠짐없이 SP-9A6D579F/PN-87D6B615로
  분리 등록돼 이미 이 감사 문서 §3에서 추적 중임을 재확인 - 갭 없음.

- **`SP-0666DB3C` §17(Mutex/Semaphore syscall 노출)**: §17.2 정정이
  `PN-CE6A04AB`/`SP-CA3C3E57`를 정확히 인용하며 "세대 태그 슬롯
  테이블" 패턴으로 교체 방향을 잡아 뒀고, `SP-E9B44929`(syscall
  그룹+call 인코딩)와는 다른 축(핸들 값 형식 vs 엔드포인트 번호
  형식)이라 서로 충돌 없음을 확인. `RM-48E1E610`의 Sync 그룹(8)도
  "§17.2 정정 대기"로 정확히 의도적 미등록 상태 유지 중 - 갭 없음.

- **`SP-04EE2A18`(Syscall 디스패치)**: 핵심 계약(submit/wait 분리,
  `SyscallRegistry`의 고정 슬롯+동적 subjectCode 간접화, 등록 안
  된 endpoint/Slab 고갈 시 블로킹 없이 실패) 전부 `syscall.h`/`.cpp`에
  그대로 구현돼 있음을 확인. `UserThread::pendingSyscall`이 원안의
  단일 필드에서 `ChunkedList` 기반 다중 슬롯(`pendingSyscalls`)으로
  진화했지만, 이는 숨겨진 이탈이 아니라 코드 주석에 명시된
  `QU-31402585`/`QU-F475C6C2`(2026-09-14, 같은 날 후속 답변)로
  확정된 정당한 확장 - `waitForMultipleSyscall`/`waitAnyForMultipleSyscall`
  까지 구현됐다. 문서가 "verb/레지스터 배치는 아직 범위 밖"이라고
  적어 둔 부분도 실제로 `kDispatchSyscallVerb`(idt.cpp)가 지금도
  verb 0(submit)/1(wait)만 처리하고 멀티웨이트용 verb는 배정 안 돼
  있음을 확인 - 문서 서술과 정확히 일치(유저랜드 자체가 아직 없어
  당장 필요하지도 않음, RM-23F4B687 §4 패턴). 갭 없음.

- **`SP-201238BB`(SharedPtr/WeakPtr/UniquePtr)**: 매우 큰 문서라
  `libkenv/shared_ptr.h`/`type_traits.h`/`spinlock.h`와 전면 대조 -
  `Atomic<T>`(AtomicU32/AtomicPtr 통합), `ControlBlockBase`/
  `ControlBlock<T,Deleter>`/`SharedPtr<T,Deleter>`/`WeakPtr<T,Deleter>`
  (별칭 생성자 포함)/`EnableSharedFromThis<T>`/`kMakeShared`/
  `UniquePtr<T,Deleter>`/`kIsBaseOf`(type_traits.h로 분리) 전부 실재.
  §2-B(`IntrusiveControlBlock`/`IntrusiveRefCounted`/
  `IntrusiveSharedPtr`/`IntrusiveWeakPtr`/`kMakeIntrusiveShared`)만
  `shared_ptr.h`에 없음 - 그러나 문서 자신이 "v1에서 실제로 적용할
  타입은 아직 미정(설계만 제공)"이라고 명시해 둔 항목이라 숨겨진
  갭이 아니라 공개적으로 유예된 설계(RM-23F4B687 §4 패턴). 갭 없음.

- **`SP-B071E628`(pubreg)**: `PN-185406F6`(pubreg 서비스 구현)이
  여전히 `scheduled`이고 `minicore/pubreg` 디렉터리 자체가 없어(git_tree
  확인) 코드 대조 자체가 성립하지 않는다 - 순수 설계 단계. 문서
  §8의 "9개 항목 전부 교차 확인 완료" 자체 선언을 표본으로 재검증
  (SP-EAB162FC §2.2의 `pubreg` 추가, `RM-48E1E610`의 `PublishInterface`/
  `QueryInterfaces` "폐기" 갱신) - 둘 다 정확함. 실제 코드 갭은 아직
  발생할 수조차 없는 단계 - 갭 없음(착수 시 재점검 대상으로 §3 유지
  가치 있음, 아래 이동 없이 여기 기록만).

### 1-F. `SP-9525C4C0`(Push/Pull 로드밸런싱) Push 경로에 §5.3 FPU 안전
가드 누락 (코드 갭, **완전 해소**)

- **출처**: 이번 스윕에서 `SP-9525C4C0`를 처음 대입 - `scheduler.cpp`와
  대조하던 중 이 문서 §5.3("이번 설계의 핵심 기여")이 확립한
  `kCanMigrateFpuSafely()` 가드가 Pull(`runLoop()`, ~1351행)에는
  정확히 있는데 **Push(`enqueue()`, ~929-943행)에는 없음**을 발견.
- **문제**: Task가 lazy FPU 소유("`gFpuOwner[coreIndex] == task`")를
  쥔 채로 Push가 다른 코어 큐로 옮기면, 원래 코어의 하드웨어 FPU
  레지스터에만 있는 최신 값이 저장되지 않고 유실될 수 있다 - §5.3이
  이미 "진짜 정합성 버그"로 명시한 바로 그 시나리오. Pull은 §5.3의
  v1 절충안(`kCanMigrateFpuSafely` 확인 후 안전하지 않으면 이관 스킵)을
  그대로 구현했지만 Push는 그 가드 없이 무조건 이관한다 - 타이밍
  의존적이라 드물게만 발현되는 데이터 손상.
- **[완료, 2026-09-17, minicore-88 세션, commit f753d19]** 원안이
  스스로 남긴 미결 질문("`coreIndex`가 항상 Task의 실제 FPU 소유
  코어와 일치하는가")을 착수 세션이 직접 조사 - `Scheduler::enqueue(`
  호출부 5곳(`debug_session.cpp`/`kmain.cpp`×2/`process.cpp`/
  `resource_group.cpp`) 전수 확인 결과 **전부 waker 자신의 코어일
  뿐 Task의 실제 FPU 소유 코어와 무관**함을 확인 - 원안의
  `kCanMigrateFpuSafely(task, coreIndex)` 재사용 방식은 채택하지
  않고, `gFpuOwner[]` 전체를 스캔해 Task의 실제 살아있는 FPU 소유
  코어를 찾는 `kFindFpuOwnerCore(const Task*)`를 신설 - Push 이관
  후보 코어가 그 소유 코어와 다르면 이번 이관을 스킵(Pull과 동일한
  보류 정책)한다. QEMU PVH(sentinel-only 경로)/GRUB SMP4(실제 스캔
  경로) 양쪽 TEMP 격리 검증 완료. `SP-9525C4C0`에도 최종 구현 반영
  완료.
- **의미**: §1-B/§1-E("onCancel 미구현")와는 다른 결함 클래스(비대칭
  가드 누락)이지만, 같은 교훈("한쪽 경로에만 적용된 안전장치가 대칭
  경로에서 빠질 수 있다")을 보여줌 - 이번 3-B 표준 절차에 "대칭 경로
  쌍은 양쪽 다 확인"이 추가된 계기. 원안이 열어 둔 미결 질문을
  착수 세션이 끝까지 추적해 원안과 다른(더 정확한) 해법으로
  귀결된 사례이기도 하다 - 설계 문서의 "확인 필요" 각주가 실제로
  후속 세션에게 정확히 전달돼 작동한 경우.
- **현재 상태**: 완전 해소.

- **`SP-6BEAE0C1`(fork/exec, SpawnProcess syscall)**: 매우 큰 문서 -
  `process.cpp`의 `SpawnProcessHandler`/`WaitHandler`와 전면 대조.
  §3의 `flags` 비트마스크(`kSpawnDebugStart` 검증+소비, `PN-A6E01B8A`/
  `PN-87D6B615` 항목8), §6의 프로세스 트리(`parent`/`children`,
  `PN-E2A114C1`의 WeakPtr 전환 포함), §4의 System V 스택 프레임
  (argv/envp가 실제로 `execImage()`에 전달돼 조립됨, `PN-E35294B8`),
  §11-3의 COW 참조 카운트 인프라(`PageFrameAllocator::retain/refCount`)
  전부 실재 확인 - 여러 세션에 걸친 다수 증분(PN-543C0CE9 등)이
  전부 정확히 교차 참조돼 있다. 갭 없음 - 대형 기능이 이 정도로
  빈틈없이 추적된 드문 사례.

- ~~**`SP-CCACB192`(libjson)**: 착수 전 순수 설계 단계라 갭 판정
  불가~~ **[갱신, 2026-09-17] 착수 완료(commit 12c7481) - §1-G로
  이동, 실제 갭 발견됨(§3 double 지원 전제가 틀림)** - 아래 §1-G
  참고.

- **`SP-F15B4A63`(지연 실행/타이머 인프라)**: 이 문서 자신이 "리액터가
  전용 Task에서 `runLoop()` 인라인 idle 경로로 재설계됐으니
  `pump()` 호출 배선도 재작업 대상"이라고 명시해 둔 항목 - 실제로
  `async_task.cpp`(`AsyncReactor::drainOnce()`의 idle 분기, ~594행)가
  `DelayedExecutionQueue::pump()`를 정확히 그 재설계된 인라인 경로에서
  호출하도록 배선돼 있음을 확인(§3의 최종 방향 그대로). 연결 리스트
  기반 `DelayedExecutionQueue`(§2, 고정 배열 폐기), Resurrect 백오프
  소비 지점(scheduler.cpp)도 실재. 갭 없음.

- **`SP-DF89897F`(커널 로깅 인프라)**: `logger.h`/`.cpp`에
  `Logger`/`LoggingDriver`/`LogLevel`/`SerialLoggingDriver`/전체
  지원 포맷터 전부 구현 확인, `kmain.cpp`(151개 호출부)/`panic.cpp`
  전부 `Logger::*`로 마이그레이션 완료 확인(`kernel::Logger::` 실제
  호출 다수 grep 확인). 유일한 미구현(`BufferedFileLoggingDriver`)은
  `PN-32696F0F` 자신이 "미착수, 범위 밖 유지 - fs 서비스 준비 후
  진행"으로 이미 공개 추적 중 - 숨은 갭 아니다. 갭 없음.

- **`SP-DABFCF9F`(QEMU gdb stub 디버깅 워크플로)**: 도구/스크립트
  문서(커널 C++ 코드 아님) - `run-qemu-gdb.sh`/`run-grub-gdb.sh`/
  `kernel.gdb` 전부 `scripts/`에 실재 확인. 문서 자신이 "설계/구현/
  실측 전부 완료"로 명시한 그대로. 갭 없음.

- **[재검증, 2026-09-17] `SP-B1E258D8`(RCU) - approved 전환 +
  `PN-495C11B7` 구현 완료 후 §5 확정 설계 대 코드 대조**: 재개 조건이
  도래한 정도가 아니라 이미 구현까지 완료됐음을 이번 틱에 확인 -
  `rcu.h`/`rcu.cpp`를 §5와 한 줄씩 대조. `RcuReadGuard`/`RcuCallback`/
  `RcuCallbackTraits`/`Rcu` 클래스 전부 실재, `noteQuiescentStateOnThisCore`
  /`startGracePeriod`/`isGracePeriodComplete`/`callAfterGracePeriod`/
  `drainCallbacksOnThisCore` 메서드 시그니처 그대로, `PerCpu<AtomicU64>
  _lastObservedSeq`/`AtomicU64 _currentSeq`/`PerCpu<List<RcuCallback,
  RcuCallbackTraits>> _pendingList`(libkcont 재사용, 손짜기 연결
  리스트 아님) 필드 구성도 일치. 소비 지점 두 곳도 직접 확인 -
  `scheduler.cpp`(preemption 카운터 0 도달 시 `Rcu::
  noteQuiescentStateOnThisCore()` 호출, ~1644행), `async_task.cpp`
  (`AsyncReactor::drainOnce()` 진입부에서 `Rcu::
  drainCallbacksOnThisCore()` 호출, ~571행 - 계획 본문이 스스로 정정한
  대로 원안의 `reactorTaskEntry()`가 아니라 `drainOnce()`로 배선됨).
  **갭 없음** - 드물게 설계-구현-검증까지 한 틱 안에 전부 정확히
  맞아떨어진 사례.

- **[재검증, 2026-09-17] `PN-2008220B`(§3 이관 항목) 해소 후
  `SP-F682B889` §7.3 재대조**: `PN-2008220B`가 completed로 전환됨에
  따라(코어당 전용 idle 스택 + `Scheduler::enterIdleLoop()` 트램폴린
  구현, commit 7e83ff1) 그 수정이 §7.3의 코루틴 재개 경로 서술과
  여전히 일치하는지 재확인. **결론: 일치, 갭 없음** - PN-2008220B의
  수정은 `runLoop()`의 idle 분기가 서 있는 **스택 자체**를 안전한
  전용 스택으로 옮긴 것일 뿐, §7.3이 서술하는 "`drainOnce()`가
  `coroHandle`이 설정돼 있으면 `coroHandle.resume()`을 직접 호출한다"
  는 호출 메커니즘 자체는 전혀 바뀌지 않았다 - §7.3은 스택 안전성을
  전제/서술한 적이 없어(그 위험 서술은 `scheduler.cpp`/`async_task.cpp`
  코드 주석에만 있었고, `PN-2008220B`가 이미 그 주석들에 정정 각주를
  남김) 애초에 낡아질 내용이 없었다. 단, §7.3이 드러내지 못했던
  "실제 CR3 값 동기화"(스택 안전성과 별개 문제)는 `PN-387C18A5`로,
  실제 co_await 핸들러 재개 시나리오 실측은 `PN-929CE93E`로 각각
  분리 추적 중 - 둘 다 openly 추적이라 숨은 갭 아님.

**[2026-09-17] approved 상태 SP 문서 후보 풀 소진** - 이 시점까지
확인 안 한 `approved` SP 문서가 더 없음(document_list로 재확인
필요시 다음 틱에). **[갱신, 2026-09-17]** §3에 있던 두 재검증 대기
항목(`PN-2008220B`/`SP-B1E258D8`) 전부 §2로 이관 완료 - §3은 이제
`SP-9A6D579F`(부분 착수 중, 완료되면 재대조) 한 항목만 남았다 -
`PN-B5C2845A`(Kill이 Syscall::wait 파킹 대상에 미도달)는 2026-09-18
완료돼 이 목록에서 빠짐(§1 참고).
다음부터는 PL/DC류 문서, 또는 새로 `approved` 전환되는 문서(예:
`SP-B26CDBDD`가 구현되면 그 시점에) 위주로 전환한다.

- **[신규, 2026-09-17] `PL-4BA2B446`(멀티 프로세스 실행 기반 완성) -
  PL류 문서 스윕 첫 사례**: 방법론을 SP/DC뿐 아니라 PL(실행 계획)류
  문서에도 처음 적용 - 이 문서는 2026-09-15 이후 자체 갱신이 멈춰
  있었지만, 그 시점에 "착수 가능"으로만 적어 둔 4단계 계획
  (`PN-0367CDBA`/`PN-B3DD3D19`/`PN-268F062B`/`PN-6D497EB0`) 전부
  이미 `completed`임을 `plan_get`으로 확인 - 코드 갭은 아니지만
  문서가 진행 상황을 못 따라간 사례(§5 기록 규칙과 같은 패턴).
  문서에 "진행 상황 갱신 3" 절을 추가해 4단계 완료 + 5단계(devmgr/fs
  등 하드웨어 착수 순서)가 이미 개별 SP 문서로 분리 추적 중임을
  반영 완료. 갭 없음(문서만 정정).

- **[신규, 2026-09-17] `PL-65C20380`(SMP AP 기동) - kApMain/kMain
  대칭 경로 점검**: 이 문서 자체는 2026-09-14 완료 기록이라 오래됐지만,
  §5(대칭 경로 확인 규칙)에 따라 그 이후 생긴 새 진입점
  (`Scheduler::enterIdleLoop()`, `PN-2008220B`)이 AP 경로(`kApMain`,
  실제로는 `smp.cpp`에 있음 - `kmain.cpp`가 아님, 문서의 오래된 서술과
  달리 파일이 옮겨진 상태이나 이건 문서 갱신 대상이라기보다 단순
  위치 정보라 각주 없이 기록만 함)에도 대칭으로 적용됐는지 직접 대조.
  `kApMain()`이 `runLoop()`을 직접 안 부르고 `Scheduler::
  enterIdleLoop()`을 부르는 것 확인, `Smp::markThisCoreOnline()`/
  `Rcu::initOnThisCore()`(`PN-907C5289`/`PN-495C11B7`가 각각 "BSP의
  kMain()과 대칭되는 지점"이라고 스스로 명시해 둔 두 호출) 둘 다
  `kApMain()`에도 실재 - **갭 없음**, 세 신규 메커니즘 전부 BSP/AP
  양쪽에 정확히 대칭 적용됨.

- **[신규, 2026-09-17] `PL-2D149D8F`(IOAPIC 외부 인터럽트 라우팅)**:
  ISO 파싱/8비트 목적지 한계/다중 IOAPIC 지원까지 3차 재작업이
  전부 반영된 상태 - 유일하게 열린 한계(다중 IOAPIC의 두 번째 이상
  인스턴스 경로, QEMU가 IOAPIC 1개만 에뮬레이트해 실측 불가)도
  `PN-6EB7F9D2`로 이미 openly 추적 중(실하드웨어 확보 전까지 정당히
  `planned` 유지). 갭 없음.

- **[신규, 2026-09-17] `PL-E68894CD`(HPET 지원)**: GSI 비트맵 라우팅/
  멀티 비교기 재작업까지 전부 반영된 상태. 유일하게 열린 항목
  (`PN-04258584`, 코어별 독립 HPET 비교기를 스케줄러와 연동)은
  그 계획 자신이 이미 "현재 스케줄러는 코어별 독립 LAPIC 타이머로
  선점을 잘 처리 중이라 실질 가치는 실측으로 필요성이 드러날 때"로
  스스로 재평가해 둔 정당한 저우선순위 백로그 - 숨은 갭 아님. 갭 없음.

- **[신규, 2026-09-17] `SP-B26CDBDD`(CPU 가중치 스케줄링/vruntime) -
  `PN-158B6B2F` 구현 완료(commit 01f5c591) 후 설계 대 코드 즉시 대조**:
  approved 직후(이 세션이 직접 설계해 4차 개정 끝에 승인받은 문서)
  구현까지 완료된 걸 확인해 바로 독립 검증 - `scheduler.cpp`를 §
  전체와 한 줄씩 대조. `TaskVruntimeTraits`(§3.1)/`kEffectiveWeightOf`
  (§2.1, clamp(100+weight,1,...))/`kVruntimeScale=1024` 고정소수점
  스케일(§2.1, `(kEffectiveWeightBase*kVruntimeScale)/kEffectiveWeightOf`
  - 이 세션이 설계 단계에서 잡은 "스케일 없으면 weight>0에서 몫이
  0으로 버려지는" 바로 그 버그의 수정이 정확히 그대로 구현됨)/§2.3
  굶주림 방지(새로 큐에 들어가는 Task의 vruntime을 큐의 현재 최솟값
  아래로는 못 내려가게 `minVruntime()`으로 보정, `OrderedList::insert`
  전에 적용)/`SetTaskWeight`(§7, `kSelfTaskWeightPid` 자기 자신
  경로만 허용, 직계 자식 경로는 `SP-30FCC8AE` 승인 전까지 항상
  `ok=false`로 명시적 거부 - 설계가 확정한 스코프 그대로)/
  `Process::memoryBytesUsed`(§6.2, `execImage()`/`destroy()`/
  Resurrect 재사용 시점 3곳 모두 그룹 합계와 함께 가산·감산) 전부
  실재 확인. **갭 없음** - 설계-승인-구현-검증이 한 세션 안에서
  전부 정확히 맞아떨어진 사례(SP-B1E258D8/RCU와 같은 급).

- **[신규, 2026-09-17] `PL-57CF86EF`(4K/2M 페이지 병합/분할)**: 경로
  (a)/(c)+분할(`Paging::mapRange`/`mergeRange`/`kSplitTwoMegabyte`,
  paging.cpp) 전부 실재 확인. 유일하게 남은 경로 (b)(임계치 기반
  사후 컴팩션)+재배치는 `PN-D28DD9F3`가 "실제 대량 4K 매핑 소비자가
  아직 없어 의미있는 임계치 계측 불가"로 정당하게 유예 - 숨은 갭
  아님. 갭 없음.

- **[신규, 2026-09-17] `PL-C8648D4D`(Channel IPC 구현) - 대칭/불변조건
  생존 점검**: 이 문서 자체(2026-09-14~16)의 "남은 것" 절은 이미
  전부 취소선으로 완료 표시돼 있어 문서 자체는 갱신이 잘 돼 있다 -
  대신 이 문서가 실측으로 발견/수정한 5개 스케줄러 동시성 버그 중
  근본 수정(버그 5, `Task::inRunQueue` 구조적 이중 스케줄링 방지
  플래그)이 그 이후 크게 재작성된 `gNormalQueues`(FIFO→vruntime
  정렬 `OrderedList`, `PN-158B6B2F`)에서도 여전히 정확히 유지되는지
  대조 - `enqueue()`/`scheduleImmediate()`의 cli-보호 확인+세팅,
  `pickNext()`의 `popMin()` 직후 해제, Pull 경로의 `insert()` 전
  해제(도둑질 실패 시 재`insert()`까지는 계속 `true` 유지) 전부
  정확히 원래 불변조건 그대로 보존됨을 확인. 갭 없음 - 대규모 스케줄러
  리팩터를 거치고도 과거에 실측으로 잡은 동시성 불변조건이 깨지지
  않은 좋은 사례.

- **[신규, 2026-09-17] `PL-21344323`(Syscall 디스패치 구현) - 문서
  정정 발견**: "남은 것" 절의 onCancel 실제 호출 경로 항목이
  "`PN-40E976F2`, Task/프로세스 종료 절차가 없어 아직 연결 불가"라고
  낡은 채 남아 있었으나, 실제로는 그 계획이 이미 2026-09-15/16에
  `completed`(commit eae13a9, `AsyncTaskState::Cancelled`+
  `AsyncTask::ownerTask`+`SelfTerminateHandler::onExec`의
  `pendingSyscalls` 사망 전파 목록 재사용)로 마무리돼 있었다 - 정정
  완료. 코드 갭 아님, 문서만 낡아 있었음.
- **[신규, 2026-09-17] `PL-1E247831`(AsyncTask 프레임워크 구현) -
  같은 낡은 서술 하나 더 발견**: PL-21344323과 완전히 동일한 문구가
  이 문서에도 그대로 복제돼 있었다(§5 기록 규칙이 경고하는 "하나
  고칠 때 다른 문서의 복제본도 확인" 패턴의 실제 사례) - 정정 완료.
  참고로 `SP-04EE2A18`(같은 계보의 설계 문서)는 이미
  "[해소, 2026-09-16, 재확인]" 절로 정확히 갱신돼 있었음(cross-check
  결과 이 세 문서 중 SP만 최신이었던 셈) - 갭 없음, PL 두 건만 문서
  정정.

- **[신규, 2026-09-17] `PL-FC38956C`(multiboot2+GRUB 부팅) - PL 스윕
  마지막 항목, 또 다른 낡은 문서 발견**: "이번엔 안 한 것" 절이
  "CPIO를 실제 initrd 마운트에 연결(PN-71C2B857) - 아직 안 함"이라고
  적혀 있었으나, 실제로는 그 계획이 2026-09-16에 `completed`(livefs
  마운트, `/sys/live/initrd.cpio` 노출) - 게다가 그 후속
  `PN-BC04D3DC`(KernelFsDriver 비동기 인터페이스 마이그레이션)까지
  완료돼 있었다. 정정 완료 - 유일하게 남은 하위 항목(실제 Open/Read
  syscall 배선, `PN-ABD23ACE`)은 `SP-2AAD7C8D` §9 착수와 함께 진행
  예정으로 이미 openly 추적 중. 코드 갭 아님, 문서만 정정.

**[2026-09-17] PL류 문서 스윕 완료** - PL-65C20380/PL-2D149D8F/
PL-E68894CD/PL-57CF86EF/PL-C8648D4D/PL-21344323/PL-1E247831/
PL-FC38956C 전부 점검(3건은 문서만 낡아 있던 정정, 나머지는 갭 없음
확인). 다음 스윕은 DC류 문서(요구분석/결정 문서) 또는 새로
approved 전환되는 문서 위주로 전환한다.

- **[신규, 2026-09-17] `DC-FB38F86F`(Paging::mapPage 동시성 락 전략
  결정) - DC류 스윕 첫 사례, 결정→구현 체인 전체 검증**: 설계자가
  (A) 주소공간별 전용 락 + higher-half 전역 락을 확정한 결정이
  `PN-90BD044E`로 실제 구현됐는지 코드 대조(`paging.cpp`의
  `gHigherHalfPagingLock`/`kLockForAddressSpaceOp`/
  `kMapPageUnlocked` 전부 실재, 재진입 회피까지 설계 그대로) - 갭
  없음. 그 검증 과정에서 파생된 두 후속 발견도 함께 확인: **①
  `PN-907C5289`**(SMP4+initrd 시나리오에서 콘솔에 안 보이는 실제
  Triple Fault - 아직 부팅 안 끝난 AP에 NMI가 도달하는 문제,
  `Smp::isCoreOnline()` 필터로 96% 감소) **② `PN-3081704A`**(남은
  4%의 근본 원인 - 두 코어가 동시에 `kPanic()`에 진입해 서로의
  stop-the-world NMI에 끼어들어 Serial 출력이 섞이는 경합,
  `kTryClaimFirstPanic()` CAS 래치로 40/40 완전 해소) - 둘 다
  `panic.cpp`/`paging.cpp`에 실제 구현 확인. 세 계획 모두 completed,
  결정→구현→실측 검증까지 전 사슬이 정확히 일치하는 좋은 사례.

### 1-G. `SP-CCACB192`(libjson) §3 "double까지 지원" - 컴파일러 제약으로
실제 구현 불가능함이 착수 중 드러남 (설계 갭, **[완전 해소,
2026-09-17, commit 586dcfc]**)

- **출처**: §3이 "(b) 정수+부동소수점 전부 지원... 이 프로젝트는
  이미 FPU 지연 컨텍스트(`PN-F258698E`)가 구현돼 있어 커널 코드에서
  부동소수점 연산 자체는 가능하다"고 확정(QU-8E75915F 답변 채택).
- **문제**: `PN-185406F6`(pubreg) 착수 중 libjson 실제 구현
  (2026-09-17, commit 12c7481)에서 **§3의 이 전제 자체가 틀렸음이
  드러났다** - `cmake/toolchain-x86_64.cmake`가 커널 빌드 전체를
  `-mgeneral-regs-only`로 컴파일해 SSE/x87 명령어 자체를 컴파일러가
  못 내게 막는다(`double` 산술이 있는 함수는 "SSE register return
  with SSE disabled" 컴파일 에러로 즉시 실패, 리턴값뿐 아니라 일반
  산술도 마찬가지 - 실측 확인). `PN-F258698E`(FPU 레지스터 상태
  저장/복원)는 Task 전환 시점의 런타임 메커니즘일 뿐 컴파일러가
  SSE 명령어를 내도 되는지와는 완전히 다른 층위 - §3 원문이 이 둘을
  혼동했다. 이 타겟/파일 단위로 `-mgeneral-regs-only`를 해제하는
  기존 패턴이 이 프로젝트에 없음도 코드 감사로 확인됨.
- **임시 우회(v1, 설계자 확정 전)**: 정수 아닌 JSON number를 double로
  계산하지 않고 원본 텍스트 그대로(zero-copy) `onNumber`에 넘김,
  `JsonWriter`도 `value(double)` 대신 `rawNumber()`(이미 포맷된
  텍스트 삽입)로 대체 - 실제 double 변환이 필요하면 그 제약이 없는
  유저랜드 호출부가 직접 수행.
- **[확정, 2026-09-17, 설계자 답변 QU-6A72AFE6]** ②(진짜 double 지원
  인프라 구축) 채택 - `-mgeneral-regs-only`를 제거하고 설계자가
  과거 커널 개발에 쓰던 플래그 세트(`-ffreestanding -O0 -nostdlib
  -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2
  -fno-exceptions -fno-rtti` 등, 필요한 것만 선별 적용)로 교체 지시 -
  `-mno-sse`류는 `-mgeneral-regs-only`처럼 부동소수점 자체를 완전히
  막는 게 아니라 MMX/SSE/SSE2를 인라인/외부 어셈블러 경로로만 국한
  시키는 것이라, 일반 C++ 코드의 `double` 산술은 컴파일러가 x87로
  처리하게 될 것으로 보인다.
- **[완료, 2026-09-17, commit 586dcfc]** `-mno-mmx -mno-sse -mno-sse2
  -mcmodel=large`로 실제 교체 완료 - **실측으로 제약이 애초 예상보다
  더 정밀하게 좁혀졌다**: x86-64 SysV ABI가 `double` 반환을 항상
  XMM0로 강제하므로, SSE 비활성 상태에서도 "함수가 double을 값으로
  반환"하는 경우만 여전히 컴파일 에러다 - `double`을 매개변수로
  받거나 함수 내부에서 계산하는 것은 x87 명령어(fldl/fadd/fstpl)로
  완전히 정상 컴파일됨을 objdump로 직접 확인. 독립 코드 검증 -
  `json.h`가 이 제약을 정확히 문서화해 뒀고(42-53행 주석), `onNumber`
  콜백/`JsonWriter::value(double)`/`kAppendDouble` 전부 `double`을
  매개변수로만 받고 반환하지 않는 관례로 실제 구현됨(§3 원안대로
  정수+부동소수점 전부 지원 복원, `rawNumber()` 임시 우회는 걷어냄).
  **갭 완전 해소.**
- **부수 발견**: 이 변경과 무관하게, SMP4+실제 3-ELF initrd(init/
  devmgr/pubreg) 조합에서 간헐적으로 여러 코어가 동시에 NMI로
  강제정지되며 Serial 출력이 섞이는 크래시가 발견됐다 - 구 플래그로
  되돌려도 동일 재현되는 사전 존재 버그로 차등 테스트로 확정,
  `PN-907C5289`/`PN-3081704A`(같은 "여러 코어 동시 kPanic" 결함
  클래스로 보이나 init+devmgr+pubreg 3-ELF 조합에서만 남은 잔여
  재현일 가능성)와 관계 확인 필요 - `PN-F7EBD6F5`로 별도 등록, 아직
  미착수(§3 다음 후보로 추가 가치 있음).

- **[신규, 2026-09-17] `DC-21647E46`(커널 전역 포인터 SharedPtr/
  WeakPtr 전환) - 5-Phase 로드맵 전체 완료 확인**: 이 문서 자체가
  이미 매우 상세히 자기 추적돼 있음(과거 이 세션이 갱신한 이력
  포함) - Phase 0-4(PN-E2A114C1/PN-21C2D4E9/PN-B41D8C0E/
  PN-B4987BF6/PN-9CC66142) 전부 completed로 정확히 기록됨. 보안
  관련 핵심 주장(Phase 4의 `kResolveOwnedBridge`가 위조된 bridge
  핸들을 막는다) 하나를 독립 재검증 - `channel.cpp`에 실재하고
  5개 syscall 핸들러 전부(`ChannelRead`/`Write`/`CloseBridge` 등)
  가 이를 통해서만 `args->bridge`를 해석함을 확인. 갭 없음 - 이
  문서가 스스로 관리한 대형 마이그레이션 로드맵이 실제 코드와
  정확히 일치하는 드문 완결 사례.

- **[신규, 2026-09-17] `DC-47000304`(DebugGetRegisters/SetRegisters
  레지스터 스냅숏 위치) - (A)안 채택 후 구현 확인**: (A)(DebugSession
  사본 저장 + write-back) 채택이 `PN-87D6B615`로 정확히 구현됨을
  코드 대조(`kSaveDebugRegistersSnapshot`/`liveFramePtr`/
  `savedRegisters` 전부 `debug_session.cpp`에 실재, write-back 로직도
  확인). 갭 없음. **부수 확인**: 이 문서가 "SMP4 검증은 별개의
  사전 존재 버그(`PN-9F8FF132`) 때문에 실행 못 함"이라고 적어 둔 그
  블로커가 **이후 완전히 해소됨**(부팅 순서 재배치, commit d720d57,
  10/10 SMP4 무결 검증) - DC-47000304 자체가 틀린 건 아니고(그
  시점엔 정확했던 서술), 이제 SMP4 하에서 DebugGetRegisters/
  SetRegisters의 완전한 E2E TEMP 재검증이 가능해진 상태(우선순위
  낮은 후속 기회로 기록만, 새 PN 등록은 보류 - RM-23F4B687 §4).

**[2026-09-17] DC류 문서 후보 풀 소진** - `DC-FB38F86F`/`DC-21647E46`/
`DC-47000304` 전부 점검 완료(갭 없음, 하나는 부수적으로 완화 기회
발견). `DC-235312EF`는 CNW 툴링(git_add_bulk 인코딩) 조사 문서라 이
방법론(설계 문서 vs 커널 코드) 대상이 아님 - 이미 자체 해소됨,
스킵. `DC-48565C0B`/`DC-427BB6B2`/`DC-79A2387A`/`DC-5AB13FFC`는
`DS-D4E5C451`에 이미 통합 반영돼 있고 그 문서 자체를 이번 세션
초반에 이미 상세 점검했으므로 개별 재확인 생략. 나머지 DC는 전부
`archived`(더 이상 인용 대상 아님). 다음 스윕은 새로 `approved`
전환되는 문서(SP-CCACB192처럼 착수와 동시에 갭이 드러나는 경우가
실제로 있었음 - §1-G 참고) 또는 QA류 문서 위주로 전환한다.

- **[신규, 2026-09-18] QA류 문서 점검 (`QA-26450C3E`/`QA-08F8C96F`) -**
  **이 방법론 대상 아님이 확인됨, 갭 없음**: QA 문서는 SP/DC처럼 "확정된
  설계"를 산문으로 선언하는 문서가 아니라 체크리스트 자체라, 이 문서의
  핵심 위험 패턴(목록 뒷부분 항목이 조용히 누락)이 애초에 잘 안 맞는다.
  실제로 대조해보니 두 문서 모두 미체크 항목마다 예외 없이 구체적인
  `PN-XXXXXXXX` 계획 참조가 이미 붙어 있어(`QA-26450C3E`의 BIOS/EFI→
  `PN-7FBF255A`, 인터럽트 라우팅/IPC→`PN-B3DD3D19`, procfs→`PN-48F0F90C`,
  devmgr/fs 실코드→`PN-BD9AAE2F`/`PN-452FF696`, 특권경계 3항목도 같은
  선행조건으로 명시; `QA-08F8C96F`의 부팅시간/컨텍스트스위칭 비용 실측→
  본문 자체가 방법론 미확정을 `CLAUDE.md` 규칙4에 따라 의도적으로 유보)
  "조용히 빠진 것"이 아니라 전부 openly 추적 중 - 갭 없음. 이 두 문서는
  이제 이 방법론의 스윕 대상 풀에서 제외(§3에도 추가 안 함), QA류는
  devmgr/fs 실코드 착수 등 실측 조건이 갖춰질 때 자연히 체크 항목이
  옮겨가는 것으로 충분.

### 1-H. `SP-1FBC0EEB`(Channel IPC) `OpenChannel`/`ConnectChannel`의
`name` 포인터 미검증 - `PN-B552E75F`(Read/Write 소급 적용)와 동일
결함 클래스가 대칭 핸들러 두 곳에 남아 있었음 (코드 갭, **발견 즉시
같은 커밋에서 해소, minicore-88 세션, commit 4eb17f1**)

- **출처**: `PN-B552E75F`(2026-09-16 completed)가 `ChannelReadHandler`/
  `ChannelWriteHandler`의 유저 포인터(`buffer`/`data`)에
  `kValidateUserBuffer()` 검증을 소급 적용했을 때, 같은 파일의
  `OpenChannelHandler`/`ConnectChannelHandler`가 쓰는 `name`/
  `nameLength`(마찬가지로 유저 포인터)는 그 소급 범위에서 빠졌다.
- **발견 경위**: `PN-EAB3A9AE`(libmc Channel IPC syscall 래퍼, 이
  프로젝트 최초의 진짜 유저랜드 Channel 호출부 준비) 착수 중,
  실제 syscall 트랩 검증을 설계하려고 `channel.cpp` 핸들러 전체를
  다시 읽다가 minicore-88이 직접 포착 - 이전까지 모든 Channel
  테스트가 내부 TEMP 스캐폴딩(가짜 UserThread가 핸들러를 직접 호출,
  실제 syscall 인자 검증 경계를 전혀 안 거침)이었기 때문에 잠복해
  있었다.
- **조치**: 발견과 동일 커밋(`4eb17f1`)에서 즉시 수정 - 두 핸들러
  모두에 `PN-B552E75F`와 동일한 `kValidateUserBuffer` 가드 추가
  (`nameLength > 0`일 때만 검사, 이름 없이 여는 `OpenChannel` 경로는
  원래도 `name`을 안 건드리므로 영향 없음). `channel.cpp` 코드로
  독립 확인 완료.
- **의미**: §1-B/§1-E/§1-F가 이미 세 번 확인한 것과 같은 일반 패턴
  ("한 결함 클래스를 한 서브시스템/경로 쌍에서 잡아도, 비슷한 시기에
  독립적으로 다뤄진 대칭 경로에는 그 교훈이 전파 안 될 수 있다")의
  네 번째 사례 - 다만 이번엔 발견부터 수정까지 같은 세션·같은 커밋
  안에서 끝나 **이 문서에 "열린 갭"으로 남은 적이 없다**(§1-B/E/F와
  달리 별도 PN 등록 없이 즉시 해소). 3-B 표준 절차("대칭 경로 쌍은
  양쪽 다 확인")가 실제로 작동을 검증받은 사례로 기록.
- **현재 상태**: 완전 해소. `PN-EAB3A9AE` 자신은 여전히 `scheduled`
  (두 개의 독립 유저랜드 프로세스 간 실제 connect/accept/read/write
  전체 핸드셰이크 실측은 아직 미완료로 명시적으로 남음) - 이건 이
  발견과 무관한 별개의 잔여 범위.

### 1-I. `SP-4DCD0E6A`(Lock-free/Concurrent 컨테이너) §3 `ConcurrentRbtree` -
실제 채택된 동시성 메커니즘이 문서에 반영 안 됨 (문서만 정정 - 코드
갭 아님, 2026-09-18)

- **출처**: 이번 스윕에서 아직 이 방법론이 다루지 않았던 approved
  SP 문서(`SP-4DCD0E6A`)를 처음 대입 - `minicore/kernel/
  concurrent_rbtree.h`(`PN-A8EF29F7`)와 대조.
- **문제**: §3 서두 산문은 `ConcurrentRbtree`의 트리 변경을 "새
  서브트리를 먼저 구성한 뒤 마지막에 원자적 포인터 1회 교체로 게시"
  (RCU식 copy-then-republish)하는 방식이어야 한다고 서술하는데, 바로
  아래 코드 스케치는 `find()`를 그냥 "락 없음"이라고만 적어 재시도
  로직조차 없다(서로 다른 두 이야기가 같은 절 안에 공존). 실제 구현
  (`PN-A8EF29F7`)은 **이 문서의 산문에도 코드 스케치에도 없는 세 번째
  방식**을 채택했다 - 착수 세션이 `RbCore::rotateLeft/rotateRight`가
  원자적 교체가 아니라 CLRS 표준 in-place 다중 필드 mutate임을 코드
  감사로 발견하고 등록한 `QU-B5CA4008`에서, 설계자가 "(A) 읽기
  seqlock류 검증/재시도"를 명시적으로 선택했다 - §3이 서술하는 "원자적
  교체"(선택 안 된 (B))는 채택되지 않았다. 실제 구현은 버전 카운터
  (홀수=쓰기 중)로 `find`/`first`/`next`가 순회 전후 버전을 비교해
  다르면 재시도(`kMaxRetries=64`)하고, `Rbtree::insert/remove`(회전
  포함) 자체는 전혀 수정하지 않는다.
- **부수 발견**: 같은 절이 "`ConcurrentRbtree`/`ConcurrentMap` 둘 다
  §1(`LockFreeList`)과 같은 논리적 마킹 삭제 방식이라 RCU가 노드 회수
  안전성 때문에 필요하다"고도 서술하는데, 실제 `concurrent_map.h`는
  마킹이 아니라 스트라이프 락을 쥔 채 직접 unlink한 뒤
  `Rcu::callAfterGracePeriod()`로 반납만 미루는 방식이고,
  `ConcurrentRbtree`가 RCU를 쓰는 이유도 노드 회수가 아니라 순수
  "읽기 도중 이 코어의 선점을 막는" 보조 용도다(이 컨테이너는
  `Rbtree`와 동일하게 `T`의 메모리를 전혀 소유/회수하지 않는 완전
  침습형이라 애초에 그런 회수 자체가 없음).
- **조치**: `SP-4DCD0E6A`에 정정 각주 추가(원문 보존) -
  `document_patch`로 §3 `ConcurrentRbtree` 코드 스케치 직후와 "착수
  불가" 문단 뒤 두 곳에 실제 채택된 seqlock 방식/RCU의 실제 용도를
  명시. 코드 쪽은 이미 `concurrent_rbtree.h` 자신의 클래스 주석이
  정확하고 정직하게(잔여 위험까지) 기록해 뒀으므로 조치 불필요 -
  **문서만 낡아 있던 것.**
- **현재 상태**: 완전 해소(문서 정정).

- **[신규, 2026-09-18] `SP-F146B7F8`(TLS/PerCpu 인프라)**: §1
  (`ThreadLocal<T>`/`TlsRegistry`)/§2(`PerCpu<T>`) 전부 `tls.h`/
  `percpu.h`와 한 줄씩 대조 - `Task::tlsSlots[kMaxTlsSlots=16]`
  (task.h:18/239), `ThreadLocal<T>::get/set`이 `Scheduler::
  currentTask()->tlsSlots[_slot]`를 그대로 씀, `PerCpu<T>::get/
  forCore`가 `_values[kAcpiMaxCpus]`+`Scheduler::currentCoreIndex()`
  를 그대로 씀 - 설계 스케치와 정확히 일치. 유일한 배치 차이
  (`PerCpu<T>`를 문서 제안 `libkenv` 대신 `kernel`에 둔 것)는 이미
  `percpu.h` 자신의 주석이 이유(get()이 kernel:: 의존 유발)까지 함께
  정확히 기록해 둠 - 숨은 갭 아니다. §2.4-1(rdtscp 기반
  `currentCoreIndex()` 전환)도 `scheduler.cpp`의 `gRdtscpSupported`
  분기(rdtscp 성공 시 그 결과, 아니면 `kScanCoreIndexByApicId()`
  폴백)로 정확히 구현돼 있음을 확인. **갭 없음.**

- **[신규, 2026-09-18] `SP-FAF768AB`(제네릭 컨테이너 템플릿 -
  Node/List/Vector/Rbtree/RbMultiTree/Map/OrderedList/LruList/Queue)**:
  이 문서는 이미 자체적으로 §6-A("착수 세션 실측 발견 - 원안 코드의
  실제 버그 2건")를 갖고 있어 위험 지대로 보고 정밀 대조했다 -
  `minicore/libs/libkcont/{intrusive_list.h, vector.h, rbtree.h,
  map.h}` 확인 결과, §6-A가 스스로 기록한 두 수정(①`List::init()`이
  `_sentinel = Node{}`(댕글링 유발) 대신 `_sentinel.prev/next = 
  &_sentinel` 개별 대입, ②`Rbtree`/`RbMultiTree::remove(T*)`가 원안의
  `static`이 아니라 인스턴스 메서드로 `_root` 갱신)가 정확히 코드에
  반영돼 있음을 직접 확인. `List`/`OrderedList`/`LruList`/`Queue`
  네 타입 전부 (파일 경로 주석이 "또는 별도 queue.h" 등으로 이미
  유연하게 열어 뒀던 대로) `intrusive_list.h` 한 파일에 통합 배치돼
  있고, `RbMultiTree`는 `rbtree.h`에 실재, `Vector<T, Policy>`의
  `DefaultContainerPolicy`/`moveElement`/`destroyElement` 훅도
  설계 그대로 구현돼 있음을 확인. **갭 없음** - 이 문서로 approved
  상태였으나 아직 이 방법론이 안 다뤘던 SP 문서 3건(`SP-4DCD0E6A`/
  `SP-F146B7F8`/`SP-FAF768AB`)을 전부 소진했다.

- **[신규, 2026-09-18] `SP-7CC5693A`(VFS 커널 서브시스템) §2.1/§2.2/§2.5 -**
  **PN-452FF696이 방금 구현한 실코드와 즉시 대조**: 이전까지는 §4-A/§9.1의
  교차 참조로만 언급되고 이 방법론이 직접 대입한 적은 없었던 문서 -
  `PN-452FF696`(VFS Mount/Unmount/ResolvePath/SignalUserlandReady/
  WaitForUserlandReady syscall 구현, commit 69d7fa9)로 처음 실코드가
  생겨 바로 대조했다. `minicore/kernel/mount_table.h`(§2.1)의
  `MountKind`/`MountEntry`/`MountTable::resolve/mount/mountKernel/unmount`
  전부 pseudocode와 정확히 일치(최장 접두사 일치 + `/` 경계 처리까지),
  `KernelFsDriver`가 `AsyncTaskHandler` 상속 형태(§2.1 2026-09-17 개정판)로
  구현된 것도 확인. `minicore/kernel/vfs_syscall.h/.cpp`(§2.2/§2.5)의
  `MountArgs`/`UnmountArgs`/`ResolvePathArgs`/`SignalUserlandReadyArgs`/
  `WaitForUserlandReadyArgs`와 5개 syscall endpoint(그룹3, `RM-48E1E610`
  갱신과 일치) 전부 문서 그대로. `SignalUserlandReady`의 "커널 전역 단
  1회만" 요구사항도 `AtomicU32::compareExchange(0,1)`로 정확히 구현.
  `ResolvePathArgs`가 `MountKind::KernelDriver`를 만나면 `NotSupported`로
  응답하는 것도 §2.1/§9.1이 이미 "§9 착수 시 확정"으로 열어 둔 것과
  일치(임의 결정 아님, CLAUDE.md 규칙4 준수를 코드 주석이 직접 인용).
  **갭 없음** - §3(드라이버 우선순위)/§9(표준 파일 API)는 이 커밋의 의도된
  범위 밖(코드 주석이 스스로 명시)이라 대상 아님, 다음 착수 시 재대조.
  minicore-88이 직접 검증 코드 안에 RM-F2DAFF66 §1-B/E를 인용해 이번
  핸들러가 그 결함 클래스와 무관한 이유까지 남겨 둔 점도 특기할 만함 -
  이 방법론의 교훈이 구현 단계에서 실제로 참조되고 있다는 방증.

**[2026-09-18] approved SP 문서 후보 풀 재소진** - document_list
전수 재대조로 찾아낸 미점검 approved SP 문서 3건을 전부 처리(1건
갭 발견/정정, 2건 갭 없음). 다음 스윕은 새로 approved 전환되는
문서 위주로 계속한다(SP-30FCC8AE(사용자/권한 체계)가 review에서
approved로 넘어가면 유력 후보 - 아직 review 상태라 대상 아님).

- **[신규, 2026-09-18] `SP-2AAD7C8D` §9(표준 파일 API) - `PN-EA4EE935`**
  **(Open/Close/Read/Write, KernelDriver 경로)와 즉시 대조**: §9.1이 이미
  스스로 "비판적 재검토로 발견한 공백"(`MountKind::KernelDriver`가 §9.2
  `FileDescriptor`에 원래 없던 판별자를 요구)으로 명시적으로 열어 둔
  자리 - `PN-ABD23ACE`(전신, PL-FC38956C의 후속) 항목2가 그 요구사항을
  이어받았고, 이번 커밋(`vfs_syscall.h/.cpp`+`process.h`)이 정확히 그대로
  구현했다. `Process::FileDescriptor::kind`(MountKind, process.h:231)가
  §9.1이 예고한 확장 그대로 실재 - 심지어 그 옆 주석이 `PN-CE6A04AB`(이
  문서가 이전에 다룬 Channel 핸들 위조 방지 보안 패턴, §2 참고)를 직접
  인용해 "임의의 정수를 그냥 믿지 않는다"는 같은 원칙을 재사용했음을
  밝혀 둠 - 이 감사 문서의 과거 발견이 실제로 후속 설계에 참조되는
  사례. `OpenArgs`/`CloseArgs`/`ReadArgs`/`WriteArgs`(vfs_syscall.h)
  전부 §9.3과 필드 단위로 일치, offset 소유권도 §9.2 그대로("offset은
  커널(fd 테이블)이 갖고 FileSystemDriver::read/write는 매번 명시적
  offset을 받는 무상태 오퍼레이션" - 코드가 `slot->value.offset +=
  bytesRead/bytesWritten`로 정확히 구현). `MountKind::Channel` 마운트는
  §9.1이 스스로 "아직 미확정, 착수 시점에 정한다"고 이미 열어 둔 대로
  `NotSupported`로 정직하게 응답(임의 결정 아님, CLAUDE.md 규칙4 코드
  주석 직접 인용) - 숨은 갭 아니다. **부수 발견**: 실측 중
  `submitterTask`를 내부 재제출 AsyncTask(KernelFsDriver 대상)에
  전파하지 않으면 `ProcFs::open()`의 "proc/self" 해석이 실패하는 실제
  버그를 찾아 4개 핸들러 전부에 전파 코드를 추가해 수정 -
  `async_task.h`의 `submitterTask` 문서 주석이 이미 예견해 둔 확장
  지점이었음을 커밋이 스스로 인용. **갭 없음** - 설계가 스스로 예고한
  공백이 정확히 그 설계 의도대로 메워진 사례(SP-9525C4C0/§1-F와 같은
  급 - 원안의 "확인 필요" 각주가 후속 세션에 정확히 전달돼 작동함).
  **[추가, 2026-09-18] `PN-238FD331`(Stat, §9.3/§9.4 call 10)도 같은 날
  후속 커밋으로 확인** - fd 없이 경로만으로 동작하는 것까지 §9.4
  규칙 그대로(ResolvePathHandler와 거의 동형), MountKind::Channel은
  Open과 동일한 스코프 결정으로 NotSupported 유지. 갭 없음, 같은
  패턴의 반복이라 별도 하위 절 없이 여기 한 줄로만 추가 기록.

- **[신규, 2026-09-18] `SP-B071E628` §6-6(pubreg register/query 완전
  바이너리) - 확정 직후 `PN-185406F6` 항목4(commit 974adce) 구현과
  즉시 대조**: 설계가 여러 차례(§6-4 절충안 반려→§6-5→QU-4B38857C
  답변→§6-6 확정) 급하게 뒤집힌 직후 착수된 구현이라 이 문서 방법론이
  가장 주목해 온 위험 패턴("승인 직후/막 구현된 문서")에 정확히
  해당 - `userland/libs/libmc/pubreg.h`의 `PubregRegisterRequest`/
  `PubregRegisterAck`/`PubregQueryRequest`/`PubregRegistrationEntry`
  전부 §6-6의 `PubregRegistration` 필드(registryId/protocolCode[4]/
  implementationId[28]/endpoint/featureFlags)와 정확히 일치,
  `PubregEndpointKind`(Channel/Tcp/Udp) discriminated union도 설계
  그대로. `minicore/pubreg/main.cpp`의 `kHandleRegister`/`kHandleQuery`가
  이 와이어 포맷을 정확히 그 레이아웃으로 파싱/조립함을 확인 -
  query의 mode 0(전체)/1(substring, `kImplementationIdMatches`)과
  offset/count 페이지네이션도 QU-E05A55AD 1번 답변 그대로. "닫힌
  파이프 → 등록 자동 해제"(`kReleaseRegistrationsOwnedBy`)도 §3/§6
  원 설계 그대로 보존. `PN-10EE096A`가 막 노출한
  `waitAnyForMultipleSyscall` 위에서 accept+다중 연결을 멀티플렉싱하는
  것도 그 syscall의 의도된 최초 소비처로 정확히 맞물림. **정직하게
  기록된 v1 단순화 하나**(코드 갭 아님, 설계 문서가 와이어 포맷까지만
  다루고 버퍼링 정책은 구현 세부로 남겨 둔 영역) - 한 `ChannelRead`가
  메시지 2개 이상을 한 번에 받아오면 첫 메시지만 처리하고
  `bytesBuffered`를 무조건 0으로 리셋해 나머지를 버린다(주석이 직접
  인정 - "다음 메시지 조각을 잃지 않으려면 별도 스크래치 필요, v1은
  파이프라이닝을 포기"); 이 프로토콜이 요청-응답 왕복이라 정상
  클라이언트는 응답 전 다음 메시지를 안 보내므로 지금은 안전하나,
  다중 메시지 파이프라이닝이 실제로 필요해지면 재검토 대상. **갭
  없음** - 설계 확정부터 구현까지 빠르게 이어졌음에도 와이어 포맷/
  페이지네이션/자동해제/신규 syscall 소비 전부 정확히 일치한 사례.

- **[신규, 2026-09-18] `PN-C39882D0`(pid ABI 마이그레이션) -
  `kFindDebuggableChild()` 놓침(코드 갭, 발견 즉시 같은 세션에서
  해소, commit 7d683d1)**: `PN-C39882D0` 자신의 문서 주석이 "이
  마이그레이션은 SpawnProcess/Wait만 다룬다 - Kill은 raw-pointer
  비교를 의도적으로 유지한다"고 명시적으로 예외를 하나만 적어 뒀는데,
  `debug_session.cpp`의 `kFindDebuggableChild()`는 그 예외 목록에
  없었음에도 여전히 옛 raw-pointer 비교를 쓰고 있었다 - 의도된
  과도기적 예외가 아니라 마이그레이션이 단순히 놓친 파일. §1-B/E/F/H가
  이미 세 번 이상 확인한 "한 결함 클래스를 고칠 때 비슷한 시기의
  다른 경로가 빠질 수 있다" 패턴의 또 다른 사례 - 이번엔 "대칭 핸들러
  쌍"이 아니라 "마이그레이션의 선언된 범위 vs 실제 커버리지" 형태.
  **영향**: 실제 `SpawnProcess`가 반환하는 `ProcessId`를 그대로
  `DebugAttach` 등에 넘기면 이 비교가 항상 실패해 `PermissionDenied`만
  반환 - PN-87D6B615의 모든 이전 TEMP 검증은 합성 `Process`+
  raw-pointer 값 조합만 써서 이 버그를 가리지 못했다(실측으로 실제
  경로를 탄 적이 없었다는 뜻). `child->processId == targetProcessId`로
  수정, 실측(4242 vs 9999 시뮬레이션 값)으로 확인. **갭 완전 해소.**
  minicore-88이 PN-87D6B615 본문에 이미 매우 상세히 자체 기록해 둠 -
  이 항목은 교차 참조용 짧은 기록.

- **[신규, 2026-09-18] `PN-49C2F890`(#DB 브레이크포인트 즉시 Blocked
  전환, commit `00fb1e7`) - 설계 대 코드 즉시 대조**: 이 세션이
  QU-396C2692(즉시 블록 지시)/QU-8172431E(IST4 공유 스택 위험, 코어당
  동시 파킹 1개 제한 정책 확정)를 직접 relay했던 바로 그 결정이
  구현까지 완료된 걸 확인해 바로 검증 - `debug_session.cpp`/`idt.cpp`
  diff를 한 줄씩 대조. `kHandleUserBreakpointHit()`이 `gDebugParkedOnCore
  [coreIndex]`가 비어 있을 때만 `Scheduler::parkCurrent()`를 직접
  호출해 즉시 파킹(정확히 지시된 메커니즘), 이미 서 있으면 기존
  지연 경로(pausedByDebugger만 세움)로 안전하게 대체 - QU-8172431E
  "코어당 동시 파킹 1개 제한" 그대로. `coreIndex`가 파킹 시점에
  지역변수로 캡처돼 있어, 재개가 로드밸런싱으로 **다른 코어**에서
  일어나도(idt.cpp 주석이 직접 명시) `gDebugParkedOnCore[coreIndex]
  = false`가 여전히 "원래 그 IST4를 점유했던" 코어의 플래그를
  정확히 내린다 - 실행 중인 코어가 아니라 점유 대상 코어 기준으로
  풀리는 것이 맞는 설계. `Scheduler::onTick()`의 기존
  `kIsPausedByDebugger()` 지연 경로(scheduler.cpp)는 제거되지 않고
  "두 번째 동시 히트" 케이스의 fallback으로 의도적으로 남겨졌다 -
  이 세션이 relay에서 "제거해도 되는지 판단 필요"로 남겨 뒀던 질문에
  구현 세션이 "유지"로 정확히 답한 셈. `kSaveDebugRegistersSnapshot()`
  도 파킹 직전으로 이동해 `DebugGetRegisters`/`SetRegisters`(§3.5)
  호환 유지. DR6 클리어를 콜백 호출 **전**으로 옮긴 동반 수정도
  재개가 다른 코어에서 일어날 수 있다는 것과 정확히 같은 근거로
  정당함(자체 발견/자체 수정, 새 설계 결정 아님 - RM 항목 대상
  아니지만 감사 과정에서 근거까지 확인). QEMU devmgr 브레이크포인트
  실측 + GRUB SMP4 스트레스 무회귀 대조까지 완료. **갭 없음.**

- **[신규, 2026-09-18] `SP-9A6D579F`(DebugSession, approved) - §목차
  나열형 대조 완료**: `PN-87D6B615`가 항목1(자료구조)/항목2(부모-자식
  권한, `kFindDebuggableChild`/`submitterTask.lock()` 사용 확인)에
  더해, 이번 세션이 직접 relay·감사한 항목3-8(브레이크포인트 설정/
  싱글스텝/정지-재개/레지스터 조회-설정/메모리 읽기-쓰기, `RM-48E1E610`
  그룹7 call0-8 전부 "구현 완료")까지 전부 단일 스레드 기준으로
  완료·검증됐다. **유일하게 남은 것은 §1-A(멀티스레드 디버깅)**인데,
  이는 설계 문서 자신이 처음부터 "프로세스가 여러 스레드를 가질 수
  있는 인프라 자체가 없다"는 선행 조건 부재를 이유로 별도 계획
  `PN-2E4E9D79`(여전히 `planned`, 미착수)로 명시적으로 분리해 뒀다 -
  숨겨진 누락이 아니라 처음부터 openly 추적된 후속 과제. **갭 없음**
  (§1-A 범위를 제외한 나머지 전부).

- **[신규, 2026-09-18] `SP-8D206F11`(CPU 캐시 관리 정책, review→approved) -
  §2 전체 대조 완료**: 이번 세션이 §2.2를 코드와 대조하다 "AP 코어도
  kMain 경로를 타 PAT MSR이 자동 적용된다"는 전제가 실제로는 틀렸음을
  발견(AP는 별도의 `kApMain`을 타고 `Paging::init()`을 거치지 않음) -
  `QU-9F758912`로 등록해 설계자가 "PAT MSR 설정을 kApMain에도 추가해"로
  직접 확정. `PN-310C870F`(commit `1a5cb29`)가 그 답변 그대로
  `Paging::initPatForThisCore()`를 BSP(`kMain`)/AP(`kApMain`) 양쪽에
  배선하고 문서 §2.2/§2.4(기존 PCD 사용처가 실제로는 인덱스2가 아니라
  인덱스3이라고 잘못 적혀 있던 것)까지 함께 바로잡아 구현 완료 - 실제
  init/devmgr/fs/pubreg initrd SMP4 15회 반복(AP 3개 전부 정상 기동)
  무회귀 확인. **갭 없음(§2 전체)** - §3(하드웨어 캐시 스누핑 여부)은
  여전히 실사용처 없어 열린 질문으로 남아 있으나 이는 설계 문서 자신이
  명시적으로 미뤄 둔 범위라 갭이 아님.

## §3. 아직 점검 안 한 영역 (다음 틱 대상)

같은 방법론(§목차 나열형 "확정된 설계" 절 vs 실제 코드)을 아직
적용 안 해본 주요 SP 문서/영역 - 매 틱 1-2개씩 골라 점검하고
결과를 이 절에서 §1(발견) 또는 §2(갭 없음)로 옮긴다:

- [ ] (`SP-9A6D579F` 항목은 §2로 이동 - 2026-09-18 갱신 완료)

(`SP-B1E258D8`(RCU) 항목은 approved 전환 + `PN-495C11B7` 구현
완료까지 끝나 아래 §2로 이동했다.)
(`PN-C4611402`의 "실제 취소 레이스" 재검증 - `PN-B5C2845A`가 열어
준 뒤 이 세션이 실제로 QEMU에서 재현/확정했다. 아래 §2로 이동.)
(`PN-2008220B` 재검증 완료 - 아래 §2로 이동.)

## §4. 예방 조치 (아직 코드가 없어 "갭"은 아니지만, 착수 시 누락 위험을
미리 체크리스트에 못박아 둔 것)

- **`SP-9DD4F3EA` §3.3a → `PN-BD9AAE2F` 항목4**: `DeviceOwnerTable`
  ↔ `PN-71C3D483`(Process Teardown Hook) 연동을 명시적 하위 항목으로
  추가(2026-09-17) - 자세한 내용은 §2의 해당 항목 참고.
  **[효과 확인, 2026-09-17]** minicore-88이 항목4(`RequestIoPermission`,
  commit e869786)를 구현하며 teardown hook 정식 연동을 지금 당장
  하지 않고 **`PN-7528A406`으로 명시적으로 분리 등록**했다 -
  `owner.lock()` 지연(lazy) GC로 임시 대체 중임과 "§3.3a가 공식
  요구사항으로 명시"했다는 사실까지 그 계획 본문에 그대로 인용해
  적어 뒀다. 예방 조치를 걸어 두지 않았다면 그냥 조용히 빠졌을
  가능성이 있는 항목이 **이번엔 미착수 상태로나마 openly 추적**된
  사례 - 이 문서(§4)의 목적이 실제로 작동함을 확인.

- **`SP-76250478`(멀티스레드 유저 프로세스 지원) → `PN-0EB2FABF`**:
  2026-09-18 approved, 구현 계획 `PN-0EB2FABF`(scheduled) 등록 완료 -
  아직 코드는 없음. §2.1(`Process::threads`/`ThreadId`/`UserThread`
  종료 필드)/§2.2(`CreateThread`)/§3 항목2-3(`SelfTerminateThread`,
  좀비/Join 정책)/§3.1(`Join`/`Detach` 진짜 블로킹, `joinerAsyncTask`)
  다섯 항목을 커밋이 올라오는 대로 실제 코드와 대조 - 특히 여러
  syscall을 한 번에 나열하는 설계라 §1-A(`Task::numaNode`)류 "뒷부분
  항목 누락" 패턴을 주의 깊게 점검한다(4개 syscall 중 일부만 반영되고
  나머지가 조용히 빠지는 경우).

## §5. 기록 규칙

- 새로 발견한 갭은 §1에 하위 절로 추가(코드 갭이면 PN 등록 +
  plan_link, 문서만 정정이면 원본에 각주만).
- **문서 하나를 정정할 때, 같은 설명이 다른 문서에도 복제돼 있는지
  의심한다** - 이 커널의 SP 문서들은 서로 참고/인용하며 같은 설계를
  자기 말로 다시 서술하는 경우가 흔하다(예: SP-8B6B8D25 §2-B와
  SP-68182FBD §2.3이 같은 유저 페이지 폴트 정책을 각자 서술) - 하나만
  고치고 나머지를 놓치면 다음 스윕에서 같은 문제를 처음 발견한 것처럼
  또 찾게 된다. 정정할 때 `docs search`/관련 문서의 backlinks로
  같은 주제를 다루는 다른 문서가 있는지 한 번 더 확인한다. **이
  복제는 CNW 문서 사이에서만 일어나지 않는다 - 소스 코드 주석도
  같은 위험에 노출된다**(2026-09-17 사례: `chunked_list.h`의 수정
  주석이 "PN-584DB994의 근본 원인일 가능성이 높다"고 최초 가설을
  그대로 남겨 뒀는데, 정작 `PN-584DB994`/`PN-A8D235E7` 두 계획
  본문은 이후 실측 근거(8회 재현 레지스터 값 완전 동일 + 이 재현
  시나리오에선 `children`/`openBridges` insert 자체가 아예 안 불림)로
  walk-back해 "근본 원인 아닐 가능성이 높다"로 뒤집었다 - 코드
  주석만 안 따라가 같은 파일 안에서 정반대 결론이 공존했다. 문서를
  walk-back할 땐 그 근거가 된 소스 코드 주석도 같이 갱신됐는지
  확인한다.
- §2/§3 사이를 오갈 때 이 문서 자체를 `plan_set`이 아니라
  `document_patch`로 갱신(문서이지 계획이 아니므로).
- **계획이 스스로 건 "착수 조건"(예: "QU-XXXXXXXX 해소 대기")을
  다시 확인할 땐 `docs pending`/`pending_list`가 아니라
  `question_list`(status=all) 또는 대상 문서의 전체 질의 스레드로
  확인한다** - `pending_list`는 설계상 미해결(open+pending)만
  보여주고 이미 `resolved`로 확인 처리된 질의는 제외하므로, "그
  질의로는 안 보인다"가 "그 질의가 애초에 없었다"를 뜻하지 않는다
  (2026-09-17 사례: minicore-88이 `PN-4190BBD3`의 "QU-C10BAA06 해소
  대기"를 `docs pending`으로만 확인해 "한 번도 등록된 적 없었다"고
  잘못 결론 - 실제로는 minicore-f8이 이미 그 답변을 확인·ack까지
  마친 상태였다, PN-4190BBD3에 정정 기록).
- 이 문서 자체는 "완결"되는 문서가 아니다 - §3이 빌 때까지, 그리고
  그 이후로도 새 SP 문서가 승인될 때마다 계속 대상에 추가한다(루프
  표준 절차 0-3번에 편입, 별도 지시 참고).

