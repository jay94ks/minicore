# CPU 가중치 스케줄링(vruntime) 및 프로세스 자원 사용량 계정 체계 — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-B26CDBDD
  status: review
  updatedAt: 2026-09-17T12:22:40.385Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 배경

설계자 opinion 2건(2026-09-17, `SP-245D130B` 대상):

> "가중치/지분(vruntime류), CPU 시간을 Task별로 집계하는 카운터 <-- 이것들 도입하자."

> "어떤 프로세스가 얼마나 썼는지 추적하는 계정 계층도 설계해서 SP로 올려줘"

`SP-245D130B`(ResourceGroup) §0이 이미 실측으로 확인해 둔 두 공백을 그대로 지시로 격상한 것이다 - "CPU 시간을 Task별로 집계하는 카운터도 없다"/"가중치/지분(vruntime류) 개념 자체가 없다"/"어떤 프로세스가 얼마나 썼는지 추적하는 계정 계층이 없다". 이 문서가 그 후속 설계다. `PN-4190BBD3`(ResourceGroup 구현) 체크리스트 항목3("CPU 쿼터 카운터 - 스케줄러 핫패스 변경이라 신중히, 별도 후속 증분")이 이미 예견해 둔 바로 그 작업이기도 하다 - 이 문서의 §1/§3이 그 항목3의 실제 설계다.

## 0. 범위 - 무엇을 다루고 무엇을 미루는가

RM-23F4B687 §4(실증된 필요 없이 복잡도를 늘리지 않음) 원칙대로 세 갈래로 나눈다:

- **지금 설계+구현**: §1(Task별 CPU 틱 카운터) / §2(가중치+vruntime 필드) / §3(Normal 클래스 큐를 vruntime 순서로 재구성) / §5(ResourceGroup CPU 계정 배선, `SP-245D130B` §3/§5가 자리만 잡아 둔 것을 실제로 채움).
- **설계만, 구현은 후속**: §6(프로세스별 메모리 사용량 coarse 계정 - `SP-245D130B` §6이 이미 스케치해 둔 `ResourceGroupMemoryControl`을 실제 갱신 지점과 함께 구체화하되, mmap 서브시스템 쪽 후킹은 별도 계획).
- **전면 보류**: 코어 간 vruntime 공정성(Push/Pull 로드밸런싱이 vruntime을 고려하게 만드는 것) - §4에서 이유와 함께 명시적으로 범위 밖 처리.

## 1. `Task` 확장 - CPU 틱 카운터 (설계자 지시 1번, 즉시 구현 가능)

```cpp
// task.h, Task 클래스에 추가
uint64_t cpuTicksUsed = 0;  // 이 Task가 실제로 Running이었던 스케줄러 틱 누적 수(100Hz, PL-2D3184BC)
```

가장 단순한 형태 - "이 Task가 지금까지 몇 틱 동안 CPU를 쥐고 있었는가"를 그대로 누적한다. 인터럽트/커널 자체 Task(리액터 idle 경로 등)에도 똑같이 적용되지만, 이 문서의 관심 대상은 유저 프로세스의 `UserThread`(`isUserLevel == true`)다. 갱신 지점은 §3에서 vruntime 갱신과 같은 자리에 묶는다(핫패스 변경을 한 곳에 모아 검증 비용을 낮춘다 - RM-23F4B687 §4).

## 2. 가중치와 vruntime - CFS(Completely Fair Scheduler) 축소판

### 2.1 `Task` 확장 필드

```cpp
// task.h, Task 클래스에 추가(§1 옆)
static constexpr uint32_t kDefaultTaskWeight = 100;  // Linux의 nice=0 기본 가중치(1024)에 대응하는 이 프로젝트의 기준값 - 정확한 스케일은 구현 시 실측 조정 가능(RM-23F4B687 §4)
uint32_t weight = kDefaultTaskWeight;  // 높을수록 더 많은 CPU 몫 - v1은 syscall로 설정하는 경로가 없다(§7 참고, 항상 기본값)
uint64_t vruntime = 0;  // "가중치로 나눈 누적 실행 시간" - 작을수록 우선 실행 대상(CFS의 핵심 불변조건)
```

`weight`/`vruntime`은 **`TaskClass::Normal`에만 의미가 있다** - `RealTime`은 `SP-9525C4C0` §6-항목4가 이미 "로드밸런싱 대상 아님"으로 확정했듯 지연시간 보장이 목적이라 공정 스케줄링 개념 자체와 무관하다(Linux도 `SCHED_FIFO`/`SCHED_RR`과 `SCHED_NORMAL`(CFS)을 분리하는 것과 같은 구분) - `gRtQueues`/`gImmediateQueues`는 이 문서로 전혀 바뀌지 않는다.

### 2.2 vruntime 갱신 공식

```cpp
// 매 스케줄러 틱마다(§3의 재스케줄 결정 지점) 현재 Running Task에 적용:
// deltaVruntime = ticksElapsed(=1) * kDefaultTaskWeight / task->weight
// vruntime이 커질수록 "이미 충분히 뛰었다"는 뜻이라 다음 pickNext()에서
// 뒤로 밀린다 - weight가 클수록(우선순위 높음) 같은 1틱에도 vruntime이
// 더 적게 늘어 상대적으로 더 자주/오래 선택된다(CFS 표준 공식 그대로,
// 정수 나눗셈이라 weight > kDefaultTaskWeight인 경우 소수점 이하가
// 버려져 정밀도가 떨어지는 점은 v1에서 허용 - 실측 후 필요하면 고정소수점
// 스케일 인수 도입 검토).
current->vruntime += 1 * kDefaultTaskWeight / current->weight;
current->cpuTicksUsed += 1;  // §1
```

### 2.3 새로 깨어난/막 생성된 Task의 vruntime - "따라잡기" 방지

CFS가 겪는 표준 문제: 방금 `SpawnProcess`로 생성됐거나 오래 블로킹돼 있다 깨어난 Task의 `vruntime`이 0(또는 오래된 값)이면, 그 큐의 다른 Task들보다 한참 작은 값이라 **당분간 계속 우선 선택돼 다른 Task를 굶길 수 있다**. CFS 원안의 해법(`min_vruntime` 기준으로 새 Task를 보정)을 그대로 축소 적용한다:

```cpp
// Scheduler::enqueue()의 Normal 클래스 분기(§3)에서, task->inRunQueue가
// false→true로 바뀌는 "새로 큐에 들어가는" 경로에서만 1회 적용(이미
// 큐 안에 있다 재정렬되는 경로는 해당 없음 - inRunQueue 불변조건은
// 기존 902-920행 로직 그대로 재사용).
if (const Task* minTask = gNormalQueues[targetCore].first()) {
    if (task->vruntime < minTask->vruntime) {
        task->vruntime = minTask->vruntime;  // 큐의 현재 최솟값까지만 끌어올림 - 더 뒤처지게(크게) 하지 않음
    }
}
```

## 3. 스케줄러 큐 재구성 - `gNormalQueues`를 vruntime 순서로

### 3.1 자료구조 교체

`gNormalQueues[coreIndex]`(기존 FIFO `TaskQueue`)를 **vruntime 오름차순으로 유지되는 `libkcont::OrderedList<Task, TaskVruntimeTraits>`**로 교체한다 - `DelayedExecutionQueue`(`SP-F15B4A63`)가 이미 이 패턴(선형 삽입 정렬 리스트)을 검증된 형태로 쓰고 있다:

```cpp
// scheduler.cpp(가칭)
struct TaskVruntimeTraits {
    using Key = uint64_t;
    static Key keyOf(const Task& t) { return t.vruntime; }
    static constexpr /* Node Task::* */ Link = &Task::vruntimeLink;  // task.h에 새 Node 멤버 추가 필요
};
using NormalQueue = OrderedList<Task, TaskVruntimeTraits>;
NormalQueue gNormalQueues[kMaxCores];
```

- `pickNext()`는 이제 `gNormalQueues[coreIndex].first()`(최솟값 = 가장 CPU를 덜 받은 Task)를 뽑는다 - 기존 FIFO `popFront()`를 대체.
- `enqueue()`는 §2.3의 vruntime 보정을 거친 뒤 `insert()`(정렬 위치 선형 탐색)한다 - 기존 `pushBack()`을 대체.
- **`approxLength()`(SP-9525C4C0 §2.1, Push/Pull 임계치 판정에 쓰임)는 그대로 유지** - `OrderedList`도 내부적으로 `List`를 감싸므로 같은 원자 카운터를 나란히 둘 수 있다(구현 세부, 착수 시 `OrderedList`에 길이 카운터를 추가하거나 별도로 유지).
- **선형 삽입 비용(O(n))**: `DelayedExecutionQueue`와 동일한 트레이드오프를 그대로 받아들인다 - 코어당 Ready Normal Task 수가 아주 많아지면 비용이 커지지만, 지금 이 커널의 실제 동시 부하 규모에서는 무시할 만하다고 판단(실측 후 재검토, RM-23F4B687 §4). 트리/힙으로 교체가 필요해지면 그건 순수 내부 구현 교체이지 이 설계 자체는 안 바뀐다.

### 3.2 갱신 지점 - `Scheduler::onTick()`의 재스케줄 분기

기존 `else { enqueue(coreIndex, current); }`(scheduler.cpp, 라운드로빈 재삽입 지점) **바로 앞**에 §2.2의 vruntime/틱 카운터 갱신을 끼워 넣는다 - `enqueue()`가 vruntime 기준으로 삽입 위치를 정하므로 갱신이 반드시 `enqueue()` 호출보다 먼저여야 한다:

```cpp
} else {
    if (current->taskClass == TaskClass::Normal && current->isUserLevel) {
        current->vruntime += kDefaultTaskWeight / current->weight;
        current->cpuTicksUsed += 1;
    }
    enqueue(coreIndex, current);
}
```

`isUserLevel` 조건은 커널 자신의 Normal Task(있다면)까지 공정 스케줄링/계정 대상으로 끌어들이지 않기 위함 - 이 문서의 관심사는 유저 프로세스 간 공정성이다.

## 4. 범위 밖 - 코어 간 vruntime 공정성 (Push/Pull 미변경)

`SP-9525C4C0`(Push/Pull 로드밸런싱)의 큐 길이 기반 판정(`approxLength()`)은 **이 설계로 바뀌지 않는다** - "코어 A의 Task 10개 합산 vruntime이 코어 B보다 낮으니 A가 더 바쁘다"는 식의 코어 간 공정성 비교는 Linux 자신도 `load_balance()`라는 별도의 무거운 서브시스템으로 다루는 훨씬 큰 문제다. v1은 "각 코어 로컬 큐 안에서만 vruntime으로 공정하게 뽑는다"는 국소적 공정성만 제공하고, 코어 간 분배는 여전히 기존 큐 길이 근사치로 판단한다 - 두 기준이 다르다는 점(길이 vs vruntime 합)에서 오는 불일치는 실사용 워크로드로 문제가 확인되면 그때 재검토(RM-23F4B687 §4).

## 5. ResourceGroup CPU 계정 배선 - `SP-245D130B` §3/§5가 남겨 둔 자리 채우기

`resource_group.h`의 `ResourceGroupCpuControl::usedTicksInPeriod`/`ResourceGroupAccounting::totalCpuTicks`는 이미 필드만 있고 갱신 코드가 없다(`PN-4190BBD3` 항목3/5, "착수 안 함"). §3.2의 갱신 지점에 이어서 채운다:

```cpp
if (current->taskClass == TaskClass::Normal && current->isUserLevel) {
    current->vruntime += kDefaultTaskWeight / current->weight;
    current->cpuTicksUsed += 1;
    // [신규, 이 문서] ResourceGroup 계정 - Process -> group 역참조 하나만 더 탄다.
    if (SharedPtr<Process> proc = /* current를 UserThread로 캐스팅 후 */ static_cast<UserThread*>(current)->process.lock()) {
        if (ResourceGroup* group = proc->group) {
            group->accounting.totalCpuTicks += 1;  // §5 - 항상 집계(쿼터 없어도 통계는 냄, SP-245D130B §5 그대로)
            if (group->cpu.periodTicks != 0) {      // §3 - 쿼터 활성 그룹만
                group->cpu.usedTicksInPeriod += 1;
            }
        }
    }
}
```

**쿼터 초과 시 실제 스로틀링**(`SP-245D130B` §3 "쿼터 초과 시 pickNext()가 그 그룹 소속 Task를 이번 주기엔 건너뛴다")은 이 문서가 새로 확정하지 않는다 - 이미 `SP-245D130B` §3이 정책을 정해 뒀고, 이 문서는 그 정책이 참조할 카운터(`usedTicksInPeriod`)를 실제로 살아있게 만드는 것까지만 다룬다. `pickNext()`가 이 카운터를 보고 건너뛰는 로직 자체는 `PN-4190BBD3` 항목3의 나머지 절반으로 그대로 남겨 둔다(이 문서 착수 세션이 원하면 같은 증분에서 이어 해도 됨 - 카운터가 살아있어야 스로틀링도 의미가 생기므로 순서상 이 문서가 선행 조건이 되는 셈).

## 6. 프로세스별 자원 사용량 계정 계층 (설계자 지시 2번)

### 6.1 CPU - 이미 §1/§5로 충족됨

"어떤 프로세스가 얼마나 썼는지"의 CPU 축은 `Task::cpuTicksUsed`(§1, 그 프로세스의 `mainThread` 하나뿐이므로 프로세스 = 그 Task, v1의 "프로세스당 스레드 하나" 전제와 일치) + `ResourceGroup::accounting.totalCpuTicks`(§5, 그룹 단위 집계)로 이미 답이 나온다 - 프로세스 단위로 더 잘게 쪼갠 개별 조회가 필요하면 `Process`에서 `mainThread->cpuTicksUsed`를 바로 읽으면 된다(새 필드 불필요).

### 6.2 메모리 - coarse 계정 (설계 확정, 구현은 후속 PN)

`SP-245D130B` §6이 이미 스케치해 둔 `ResourceGroupMemoryControl{limitBytes, usedBytes}`는 **그룹 단위**였다 - "어떤 프로세스가"라는 질문에 답하려면 `Process` 자신에게도 대응 필드가 필요하다:

```cpp
// process.h, Process 클래스에 추가
uint64_t memoryBytesUsed = 0;  // coarse - 아래 갱신 지점에서만 변함, 실시간 페이지 단위 아님
```

**갱신 지점(coarse - 정확한 페이지 단위 실시간 추적이 아니라 큰 단위 이벤트에서만)**:

- `SpawnProcessHandler::onExec`이 `execImage()` 성공 후 로드된 이미지 크기(§4 스택 프레임 크기 포함)만큼 가산.
- 향후 `mmap` 서브시스템(`SP-2AAD7C8D`)이 실제로 페이지를 매핑/해제할 때 그 크기만큼 가산/감산 - **이 배선은 이 문서 범위 밖**(mmap 쪽 후킹 지점은 그 서브시스템을 직접 다루는 후속 계획이 결정, CLAUDE.md 규칙 4 - 남의 서브시스템 세부를 이 문서가 대신 정하지 않는다).
- `Process::destroy()` 시점에 그 프로세스가 속한 `ResourceGroup::accounting`에서 이 프로세스 몫을 감산(그룹 합계가 이중 계산되지 않도록) - 정확한 감산 시점(destroy 초입 vs 각 자원 해제 완료 후)은 착수 세션이 실측 확정.

`totalMemoryBytesUsed`를 `ResourceGroupAccounting`에도 추가해(`totalCpuTicks` 옆) 그룹 합계를 유지한다 - 개별 `Process::memoryBytesUsed`의 단순 합.

## 7. 범위 밖 - 가중치 설정 API (syscall)

`Task::weight`를 유저랜드가 바꿀 수 있는 syscall(`SetTaskWeight`류)은 이 문서에서 다루지 않는다 - v1은 모든 Task가 `kDefaultTaskWeight`로 고정되어 **CPU 공정성 자체(모두 동일 가중치로 라운드로빈과 동등하게 동작)만 성립**한다(사실상 이번 증분의 실질 효과는 "라운드로빈 → vruntime 기반 라운드로빈"이라는 내부 구현 교체 + 카운터/계정 신설이지, 가중치 차등 자체는 아직 아무도 안 씀). `SP-245D130B` §9-5("CPU 우선순위 부여를 언제 쿼터 이진 구분에서 상대 가중치로 격상할지")가 이미 이 확장을 별도 열린 질문으로 걸어 뒀다 - `ResourceGroup` 단위 가중치(그룹 전체의 상대 몫)로 노출할지, `Task`/`Process` 개별 가중치로 노출할지는 실사용처가 생기면 그때 확정(RM-23F4B687 §4).

## 8. 새로 등장하는 용어 (RM-32D06563 §12 규칙 대상)

`Task::weight`/`Task::vruntime`/`Task::cpuTicksUsed`/`TaskVruntimeTraits`/`Process::memoryBytesUsed`/`ResourceGroupAccounting::totalMemoryBytesUsed`를 RM-32D06563에 등록 필요(승인 후 반영).

## 9. 검증 계획 (착수 시)

1. **vruntime 공정성**: QEMU에서 서로 다른 개수의 CPU-bound 진단 Task(전부 기본 `weight`)를 동시에 스폰해, 장시간 실행 후 `cpuTicksUsed`가 서로 근사하게 균등한지 확인(완전히 동일하진 않음 - 선형 삽입 정렬의 근사적 성질 때문에 약간의 편차는 정상).
2. **새 Task 굶주림 방지**(§2.3): 이미 오래 실행 중인 Task들이 있는 코어에 새 Task를 스폰했을 때, 그 새 Task가 합리적인 시간 안에 최초로 선택되는지 확인(무한정 뒤로 밀리지 않는지).
3. **RT/Immediate 클래스 무영향**: 기존 `gRtQueues`/`gImmediateQueues` 관련 시나리오(있다면) 전부 무회귀.
4. **ResourceGroup 계정**: 그룹에 가입된 프로세스의 `totalCpuTicks`가 실제 실행 시간과 비례해 증가하는지, 그룹에 없는(루트) 프로세스도 `gRootResourceGroup.accounting`에 정상 집계되는지.
5. QEMU 4개 표준 시나리오 무회귀(스케줄러 핫패스 변경이라 특히 신중히 - `PN-4190BBD3` 항목3이 이미 이 위험을 경고해 둔 그대로).

## 10. 착수 조건

없음 - `Task`/`ResourceGroup`/`OrderedList`(libkcont, 이미 구현) 전부 이미 존재한다. `approved` 전환 후 구현 계획(PN)을 별도 등록해 진행한다(CLAUDE.md 규칙 7) - 스케줄러 핫패스 변경이라 `PN-4190BBD3`과 마찬가지로 신중한 단일 증분으로 진행할 것을 제안한다.
