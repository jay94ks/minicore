# AsyncTask 코어 간 완료 이관(cross-core completion) — Push/Pull 로드밸런싱 확장 설계 제안 (SP-F682B889 §2 비목표 해소, PN-2CD26587)

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-BF0B31B5
  status: approved
  updatedAt: 2026-09-26T09:38:07.105Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 0. 배경

`SP-F682B889`(커널 전용 비동기 프레임워크) §2가 v1 비목표로 명시한
"코어 간 완료 이관"(코어 A에서 완료된 AsyncTask를 코어 B의 리액터가
대신 처리하는 것은 지원 안 함)을 `PN-2CD26587`이 추적해 왔다 -
"Push/Pull 로드밸런싱(`PN-7679813D`)이 실제로 AsyncTask를 코어 간
이관시키기 시작하면 재검토"가 착수 조건이었는데, 실제 구현된
Push/Pull(`SP-9525C4C0`)은 `kernel::Task`(Ready 큐)만 대상으로
하고 AsyncTask는 명시적으로 제외했다 - 그래서 이 계획은 "여전히
planned"로 계속 유지돼 왔다.

## 1. 코드 재조사로 발견한 것 - 이미 있는 인프라 (2026-09-26)

`minicore/kernel/async_task.h`/`.cpp`를 다시 읽어 확인한 결과,
이 문제의 **일부**는 이미 해소돼 있었다:

- `AsyncTask::homeCoreIndex`(제출된 코어) / `AsyncTask::
  allowCoreMigration`(기본값 false) 필드가 이미 존재한다
  (`PN-622BA93C`/`QU-FDB32CCE`, 설계자 답변 "옵션2"로 2026-09-16에
  이미 결정·구현됨).
- `AsyncReactor::submitCompletion(task, preemptive)`
  (`async_task.cpp:948-960`)의 대상 코어 계산이 이미 이 필드를
  반영한다: `allowCoreMigration`이 true면 **완료가 일어난 현재
  코어**에 그대로 큐잉하고, false(기본값)면 항상 `homeCoreIndex`로
  라우팅한다.
- Channel IPC(`channel.h`)의 대기자 깨우기도 같은 `submitCompletion()`
  경로를 타므로, 이 옵트인이 이미 일관되게 적용된다 - `PL-C8648D4D`
  가 걱정했던 "완료 시점의 그 코어에서만 대기자를 깨움" 전제도 이
  필드로 이미 완화될 수 있는 상태다.

**즉 "완료 알림이 어느 코어 큐에 들어갈지"는 이미 AsyncTask 자신이
옵트인으로 선택할 수 있다.** 이 SP가 실제로 다루는 남은 공백은
다르다.

## 2. 진짜 남은 공백 - "이미 큐에 대기 중인 AsyncTask"의 강제 이관

`allowCoreMigration`은 **완료하는 쪽**(완료 알림을 발생시키는
코드)이 스스로 선택하는 옵트인일 뿐이다. Push/Pull이 `kernel::Task`
Ready 큐에 대해 하는 일 - **바쁜 코어의 큐에 이미 쌓여 있는 항목을
로드밸런서가 외부에서 강제로 한가한 코어로 옮기는 것** - 의
AsyncTask 버전은 없다.

**왜 지금까지 필요 없었나**: AsyncTask는 대개 아주 짧게 살고(디스크
I/O 완료 대기 등) 리액터가 매 틱 빠르게 드레인하므로 큐 불균형이
누적될 일이 적었다. `AsyncCoroMutex` 기반 대기열(`PN-6D2C8836`/
`PN-CA92C4A7`)이 새로 생기면서, 특정 코어에 AsyncTask가 몰리는
워크로드(예: 한 inode-table 블록에 경합이 몰림)가 실측으로 처음
나타났다 - 지금은 억지로 안 만들면 안 보이지만, 실사용 워크로드가
늘면 잠재적 병목이 될 지점이다.

## 3. 설계 - 두 종류의 AsyncTask를 반드시 구분한다 (핵심 통찰)

- **코루틴 기반**(`co_return`이 있는 진짜 C++20 코루틴 -
  `Ext4Driver::onExec` 등): 상태가 힙에 할당된 코루틴 프레임에
  있고 특정 코어의 물리 스택에 묶여 있지 않다. `gExecQueues`/
  `gPreemptiveQueues`의 큐 노드를 다른 코어의 같은 큐로
  `pushBack()`하는 것만으로 안전하다 - 단, 그 코루틴 몸체가
  코어 로컬 상태(`gCurrentAsyncTask[coreIndex]` 등)를 참조하지
  않는다는 전제가 필요하다(이미 `allowCoreMigration`이 요구하는
  것과 같은 전제).
- **스택풀 기반**(`AsyncTask::yield()`/`kContextSwitch` 경로 -
  리액터의 전용 스택 슬롯을 실제로 쓰는 쪽): 그 코어의 물리 스택
  (레지스터 저장 포함)에 묶여 있어 이관이 안전하지 않다 -
  `SP-83A07867`이 다루는 Task의 CR3/FPU/TSS 동기화 문제와 같은
  급의 비용이 든다. **이 SP는 스택풀 기반 AsyncTask의 이관을
  명시적으로 비목표로 유지한다** - 이 프로젝트의 스택풀 AsyncTask
  사용처 자체가 적고, 이관 비용이 실질적 이득보다 크다고 판단.

### 3.1 구분 메커니즘 - 착수 세션이 확인할 것

`AsyncTask` 구조체에 "코루틴이냐 스택풀이냐"를 나타내는 필드가
이미 있는지, 없다면 이 SP의 최소 배선으로 새 `bool isCoroutine`
(또는 반대 의미) 필드 하나를 추가해야 하는지 - **착수 세션이 코드
대조로 먼저 확인**한다(CLAUDE.md 규칙4 - 확인 안 된 것을 확정으로
적지 않음). 이 SP는 "이 구분이 가능해야 한다"는 요구사항만 확정
한다.

### 3.2 구체 메커니즘

1. `AsyncTaskQueue`에 `TaskQueue::approxLength()`(Push/Pull이
   `kernel::Task` Ready 큐에 이미 쓰는 것)와 같은 이름/시그니처
   관례로 `approxLength()`를 추가한다.
2. 리액터 루프(`AsyncReactor::drainOnce()`의 상위 호출자, 또는
   idle 진입 직전)가 주기적으로 자기 코어의 `gExecQueues[coreIndex].
   approxLength()`가 임계치를 넘는지 확인한다.
3. 넘으면, 큐 맨 앞부터 살펴 **코루틴 기반이고 `allowCoreMigration
   ==true`인 항목만** 골라(스택풀 기반은 절대 건너뛰지 않고 무시)
   가장 한가한 코어를 찾아 `popFront()`+그 코어의 같은 큐로
   `pushBack()`한다(Push 방향).
4. 대칭으로 idle 코어가 바쁜 코어에서 훔쳐오는 Pull 방향도 같은
   조건으로 지원한다.
5. **임계치/스캔 주기의 구체적 수치는 이 설계에서 확정하지 않는다**
   - Push/Pull(`SP-9525C4C0` §2)이 이미 "설정 가능한 최대/최소값 +
   코어 수 대비 상대적 기준"(`QU-9325BD40` 답변)으로 확정해 둔
   패턴을 그대로 재사용할 것을 권장하되, 정확한 임계치는 착수 세션이
   실측하며 정한다.

## 4. 검토했으나 채택하지 않은 대안

- **Push/Pull(`SP-9525C4C0`)을 직접 확장해 Task와 AsyncTask를 같은
  로드밸런서가 통합 처리**: Task 이관(CR3/FPU/TSS 동기화 필요,
  `SP-83A07867`)과 코루틴 AsyncTask 이관(단순 큐 노드 이동)은
  안전 조건과 비용이 근본적으로 다르다 - 같은 로직에 억지로 합치면
  "이 항목이 Task인지 AsyncTask인지"를 매 스캔마다 분기해야 해
  오히려 더 복잡해진다고 판단, 완전히 독립된 별도 스캔 루프로
  설계한다.
- **`allowCoreMigration`을 로드밸런서가 직접 뒤집어 강제 이관**:
  이 필드는 "완료 알림이 어디로 갈지"를 결정하는 용도로 이미 의미가
  확정돼 있다 - 로드밸런서가 이걸 남용해 대기 중인 항목을 건드리면
  기존 의미와 혼동을 일으킨다. §3.2처럼 별도 스캔+이동 메커니즘으로
  분리한다.

## 5. 참고
- `SP-F682B889` §2 - v1 비목표 원문.
- `SP-9525C4C0`/`PN-7679813D`(completed) - Push/Pull 로드밸런싱(Task 전용) 원 설계.
- `PN-2CD26587` - 이 설계를 요청한 백로그 계획.
- `minicore/kernel/async_task.h:366-381` - `homeCoreIndex`/`allowCoreMigration` 원 설계.
- `minicore/kernel/async_task.cpp:948-960` - `AsyncReactor::submitCompletion()`의 대상 코어 계산.
- `PN-6D2C8836`/`PN-CA92C4A7` - 이 공백을 실측으로 드러낸 최근 `AsyncCoroMutex` 대기열 워크로드.
