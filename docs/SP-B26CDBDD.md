# CPU 가중치 스케줄링(vruntime) 및 프로세스 자원 사용량 계정 체계 — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-B26CDBDD
  status: review
  updatedAt: 2026-09-17T12:41:37.893Z
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

**[정정, 2026-09-17, 설계자 opinion]** "SetTaskWeight류도 포함해줘. Weight값의 범위는 -100 ~ 100(정수)야. 기본값은 0. 즉, 상대 가중치를 도입한다는 결정이야." - 최초안의 부호 없는 절대 가중치(항상 양수, 기본 100)를 폐기하고, **부호 있는 -100~100 상대값**(기본 0 = 기준)으로 전면 교체한다. §7이 "범위 밖"으로 미뤘던 가중치 설정 syscall도 이번 v1에 포함된다(아래 §7 참고) - "상대 가중치를 도입한다"는 명시적 결정이므로 이 문서는 더 이상 "실질 효과는 라운드로빈 교체뿐"이라고 서술하지 않는다.

```cpp
// task.h, Task 클래스에 추가(§1 옆)
static constexpr int32_t kMinTaskWeight = -100;
static constexpr int32_t kMaxTaskWeight = 100;
static constexpr int32_t kDefaultTaskWeight = 0;   // 설계자 확정값 - 기준(基準), 음수=덜 받음/양수=더 받음
int32_t weight = kDefaultTaskWeight;               // 유저가 직접 보는 값(§7 SetTaskWeight로 설정) - 항상 [-100, 100]
uint64_t vruntime = 0;  // "실효 가중치로 나눈 누적 실행 시간" - 작을수록 우선 실행 대상(CFS의 핵심 불변조건)
```

**표면 값(-100~100) → vruntime 계산용 실효 가중치 변환**: vruntime 공식(§2.2)은 CFS 표준대로 나눗셈의 분모가 항상 양수여야 한다(0 또는 음수는 무의미/미정의) - 그래서 표면의 부호 있는 `weight`를 그대로 나눗셈에 쓰지 않고, 항상 양수인 **실효 가중치**로 한 번 변환한다. v1은 가장 단순한 선형 변환을 채택한다(RM-23F4B687 §4 - 정확한 곡선은 실측 후 조정 가능, 지금은 검증 가능한 가장 단순한 형태로 시작):

```cpp
// scheduler.cpp(가칭) - 표면 weight[-100,100] -> 실효 가중치[1,200]
// (선형: effectiveWeight = 100 + weight, 0으로 나누기 방지를 위해
// 최솟값 1로 클램프 - weight=-100은 이론상 0이 되지만 그러면 나눗셈이
// 무의미해지므로 "거의 안 받음"에 해당하는 1로 바닥을 둔다.)
constexpr int32_t kEffectiveWeightBase = 100;  // weight=0(기본값)일 때의 실효 가중치 - 기존 kDefaultTaskWeight 자리를 그대로 계승
constexpr int32_t kMinEffectiveWeight = 1;
int32_t kEffectiveWeightOf(int32_t weight) {
    const int32_t raw = kEffectiveWeightBase + weight;  // [-100,100] -> [0,200]
    return raw < kMinEffectiveWeight ? kMinEffectiveWeight : raw;
}
```

**[정정, 2026-09-17, 설계자 opinion(QU-DA6C52BA 답변)]** "범위를 -100~100을 줬다면 수학적 동치값이라는 개념으로 이를 연산해서 정밀도가 높은 것처럼 연산하는 트릭을 활용할 수 있어." - `kEffectiveWeightBase / kEffectiveWeightOf(weight)`를 그대로 정수 나눗셈하면 실효 가중치가 `kEffectiveWeightBase`(100)보다 큰 모든 경우(즉 `weight > 0`인 모든 양수 가중치, 정확히 이 기능이 존재하는 이유인 케이스)에서 몫이 소수(0.xx)라 정수 나눗셈이 **0으로 버려진다** - "가중치를 더 준" 효과가 반대로 완전히 사라지는 실질적 결함이었다. 고정소수점(fixed-point) 스케일 인수로 분자를 먼저 크게 키워 두면(나눗셈 결과가 정수라도 원래 실수 몫의 정보를 그 배율만큼 보존) 이 손실이 사라진다 - "수학적 동치값"이 가리키는 것이 정확히 이 트릭이다(`a/b`가 잃는 소수부 정보를 `(a*S)/b`로 스케일업해 정수 연산 그대로 보존, 이후 비교 연산이 전부 같은 스케일 `S` 위에서 일관되게 이뤄지는 한 상대적 순서는 실수 연산과 수학적으로 동치):

```cpp
// vruntime 자체를 스케일된 단위로 다룬다(모든 Task가 항상 같은 스케일을
// 쓰므로 OrderedList의 상대 순서 비교(§3)는 전혀 영향받지 않는다 -
// "값 자체"가 아니라 "값들 사이의 순서"만 의미가 있다는 CFS 불변조건과
// 정확히 부합). 2의 거듭제곱을 골라 나눗셈이 필요하면 시프트로도 대체
// 가능하게 했다(컴파일러가 상수 나눗셈을 어차피 시프트/곱셈으로 최적화
// 하지만 의도를 명시).
constexpr uint64_t kVruntimeScale = 1024;  // 2^10 - 실효 가중치 최댓값(200)까지도 몫의 소수부 10비트 이상을 보존
```

이 스케일을 §2.2/§3.2/§5의 갱신 공식 전부에 곱해 넣는다(아래 각 절 코드 갱신 완료) - `current->vruntime += (kEffectiveWeightBase * kVruntimeScale) / kEffectiveWeightOf(current->weight);`. `kEffectiveWeightOf(weight)=200`(weight=100, 최대)이어도 `100*1024/200=512`로 여전히 0이 아닌 유효한 증분을 얻는다(스케일 없이는 `100/200=0`으로 완전히 소실됐던 바로 그 경우) - 이 문서가 원래 "v1에서 허용"이라고 넘겼던 정밀도 손실이 사실은 "허용 가능한 근사"가 아니라 "가중치 상향 기능 자체가 통째로 무효화되는" 실질적 버그였다는 것이 이번 정정의 핵심.

`weight=0`(기본값)은 `effectiveWeight=100`으로 기존 상수와 정확히 같은 값이라 §2.2/§5의 나눗셈 공식 자체는 안 바뀐다 - `task->weight`를 쓰던 자리를 `kEffectiveWeightOf(task->weight)`로만 바꾸면 된다. 이 선형 곡선은 최댓값(200)이 최솟값(1)의 200배 CPU 몫을 갖는다는 뜻 - 극단값 사이 격차가 필요 이상으로 크거나 작다고 실사용에서 판단되면 곡선(예: 지수 스케일)을 재검토한다(지금은 근거 없이 더 복잡한 곡선을 미리 만들지 않는다, RM-23F4B687 §4).

`weight`/`vruntime`은 **`TaskClass::Normal`에만 의미가 있다** - `RealTime`은 `SP-9525C4C0` §6-항목4가 이미 "로드밸런싱 대상 아님"으로 확정했듯 지연시간 보장이 목적이라 공정 스케줄링 개념 자체와 무관하다(Linux도 `SCHED_FIFO`/`SCHED_RR`과 `SCHED_NORMAL`(CFS)을 분리하는 것과 같은 구분) - `gRtQueues`/`gImmediateQueues`는 이 문서로 전혀 바뀌지 않는다.

### 2.2 vruntime 갱신 공식

```cpp
// 매 스케줄러 틱마다(§3의 재스케줄 결정 지점) 현재 Running Task에 적용:
// deltaVruntime = ticksElapsed(=1) * kEffectiveWeightBase * kVruntimeScale / kEffectiveWeightOf(task->weight)
// vruntime이 커질수록 "이미 충분히 뛰었다"는 뜻이라 다음 pickNext()에서
// 뒤로 밀린다 - 실효 가중치가 클수록(양의 weight, 우선순위 높음) 같은
// 1틱에도 vruntime이 더 적게 늘어 상대적으로 더 자주/오래 선택된다
// (CFS 표준 공식 그대로). kVruntimeScale(§2.1 정정 참고)로 분자를 먼저
// 키워 둔 덕분에 실효 가중치가 kEffectiveWeightBase보다 큰 경우(양의
// weight - 이 기능이 존재하는 이유인 바로 그 경우)에도 몫이 0으로
// 버려지지 않는다.
current->vruntime += (kEffectiveWeightBase * kVruntimeScale) / kEffectiveWeightOf(current->weight);
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
        current->vruntime += (kEffectiveWeightBase * kVruntimeScale) / kEffectiveWeightOf(current->weight);
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
    current->vruntime += (kEffectiveWeightBase * kVruntimeScale) / kEffectiveWeightOf(current->weight);
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

## 7. `SetTaskWeight` syscall - 가중치 설정 API (설계자 지시로 v1에 포함)

**[정정, 2026-09-17, 설계자 opinion]** 최초안은 이 syscall을 범위 밖으로 미뤘으나, 설계자가 명시적으로 "SetTaskWeight류도 포함해줘... 상대 가중치를 도입한다는 결정"이라고 확정했다 - v1에 포함한다. `SP-245D130B` §9-5("쿼터 이진 구분 → 상대 가중치로 언제 격상할지")도 이 결정으로 함께 해소된다.

### 7.1 범위 - 자기 자신 + 직계 자식(uid 권한 우위 조건부)

**[정정 2차, 2026-09-17, 설계자 opinion]** "부모 프로세스의 uid가 가진 권한이 자식 프로세스보다 크면, SetTaskWeight를 부모가 사용할 수도 있어야해." - 자기 자신뿐 아니라 **직계 자식**(process 트리, `Process::children`)도 대상이 될 수 있다 - 단 호출자의 uid가 대상의 uid보다 "권한이 큰" 경우로 조건이 걸린다.

```
SetTaskWeight(targetPid: int64_t, weight: int32_t) -> error
// targetPid == kSelfPid(-1, 예약값) -> 항상 허용, 호출자 자신.
// targetPid == 호출자의 직계 자식의 실제 pid -> 아래 권한 검사 통과 시에만 허용.
// 그 외(직계 자식이 아니거나 존재하지 않는 pid) -> 항상 거부(NotFound류).
```

**"uid가 가진 권한이 크다"의 정의 - `SP-30FCC8AE` §1-A 재사용**: 호출자 `caller.uid`가 `kRootUid`이거나, 대상 `target.uid`의 uid 트리 조상이면("caller가 target보다 위") 허용한다 - `SP-30FCC8AE` §1-A `kSetuid()`가 이미 쓰는 것과 **정확히 같은 판정**(`caller.uid == kRootUid || kIsDescendantUser(target.uid, caller.uid)`). 새 권한 모델을 이 문서가 발명하지 않고, 이미 있는 uid 트리 규칙을 그대로 빌려 쓴다 - "누가 누구보다 위인가"라는 질문 자체가 `SP-30FCC8AE`가 이미 정의해 둔 개념이기 때문이다.

**대상 범위를 "직계 자식"으로 제한하는 이유(임의 pid 아님)**: **[확정, 2026-09-17, 설계자 답변(QU-C1265CF0)]** "v1 뿐만 아니라 항상 직계 자식만 허용되는 동작이야" - 이 제한은 `kResolveProcessId`(`SP-9CB55C5B`, 아직 미완성)가 준비될 때까지의 임시 축소가 아니라 **영구 정책**으로 확정됐다. (참고로 `kResolveProcessId` 미완성이 지금 당장 임의 pid를 다루지 못하는 실무적인 이유이기도 하다 - `Kill`(`PN-71E50394` 항목4)이 이미 쓰고 있는 것과 같은 방식(호출자의 `Process::children`을 순회해 일치하는 pid만 찾는다, 별도 안전 해석 인프라 불필요)을 그대로 재사용한다.) 부모-자식은 스폰 시 uid를 상속하므로(`SP-30FCC8AE` §1) 보통은 uid가 같아 이 권한 검사를 통과 못 하지만, 자식이 스스로 `kSetuid()`로 하위 uid로 전환한 뒤라면 부모가 여전히 조상이라 검사를 통과한다 - 바로 이 설계자 지시가 노리는 시나리오(부모가 더 낮은 uid로 내려간 자식의 가중치를 계속 조정할 수 있어야 함).

**하드 의존성 - `SP-30FCC8AE` 승인 전까지 이 확장은 구현 불가**: `kIsDescendantUser`/`Uid`/`Process::uid` 전부 그 문서가 소유한 아직 미승인(`review`) 설계다 - 이 문서는 그 문서가 이미 확정한 판정을 "재사용"만 할 뿐 새로 정의하지 않으므로 설계 문서 차원에서는 지금 확정할 수 있지만, **실제 구현은 `SP-30FCC8AE`가 approved로 전환되고 `Uid`/`kSetuid`/`kIsDescendantUser`가 실제 코드로 존재한 이후에만 가능**하다 - 착수 세션이 `SP-30FCC8AE` 관련 계획에 `plan_depend`를 걸 것. 그 전까지는 자기 자신 대상 경로만(§7.2 핸들러의 `targetPid==kSelfPid` 분기) 구현 가능.

### 7.2 검증 및 핸들러 스케치

```cpp
// scheduler.cpp 또는 별도 파일(가칭) - SP-04EE2A18 관례(AsyncTaskHandler 하나 = syscall 엔드포인트 하나) 그대로.
constexpr int64_t kSelfPid = -1;  // targetPid 예약값 - "호출자 자신"

struct SetTaskWeightArgs {
    int64_t targetPid = kSelfPid;
    int32_t weight = 0;
    // out
    bool ok = false;
};

class SetTaskWeightHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SetTaskWeightArgs*>(argsRaw);
        if (args->weight < Task::kMinTaskWeight || args->weight > Task::kMaxTaskWeight) {
            args->ok = false;  // 범위 밖 - 조용히 clamp하지 않고 거부(표준 커널 syscall 관례, §3의 flags 검증과 동일한 정신)
            co_return;
        }
        SharedPtr<Task> submitter = task->submitterTask.lock();
        if (!submitter) {
            args->ok = false;
            co_return;
        }
        if (args->targetPid == kSelfPid) {
            submitter->weight = args->weight;  // 항상 허용 - 다음 vruntime 갱신부터 즉시 반영
            args->ok = true;
            co_return;
        }
        // [SP-30FCC8AE 승인 이후 구현 가능 - §7.1 "하드 의존성" 참고]
        // Kill(PN-71E50394 항목4)과 동일한 v1 스코프로 직계 자식만 순회해 찾는다
        // (kResolveProcessId 불필요) - 찾으면 uid 권한 검사(caller.uid==kRootUid
        // || kIsDescendantUser(child->uid, caller->uid), SP-30FCC8AE §1-A 재사용)
        // 통과 시에만 그 자식 Task의 weight를 설정한다. 못 찾거나 권한 검사
        // 실패면 ok=false.
        // (의사코드 - Process::uid/kIsDescendantUser가 실제 존재할 때 구현)
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}  // onExec에 정지 지점 없음 - SP-71DA77B3/PN-BD276A24가 확립한 판단 기준과 동일
};
```

**이미 큐 안에 있는 동안 가중치가 바뀌면**: `OrderedList`(§3)는 삽입 시점의 `vruntime` 값으로 위치가 고정된다 - 가중치 변경은 그 Task의 **다음** vruntime 갱신(§2.2, 다음에 실행되고 다시 재큐잉될 때)부터만 새 가중치를 반영한다. 이미 큐에 있는 위치를 즉시 재계산해 재정렬하지 않는다(그러려면 그 Task를 큐에서 찾아 제거 후 재삽입해야 하는데, 지금 당장 실행 중이 아닌 대기 중 Task 하나를 골라 바꾸는 빈도 자체가 낮을 것으로 예상돼 v1에서는 이 지연을 허용 - RM-23F4B687 §4).

### 7.3 RM-48E1E610 번호 예약

`kMakeSyscallEndpointId(0, 5)` - 그룹 0(Process)의 다음 미사용 call 번호(현재 0-4는 SelfTerminate/Kill/SignalAction/Wait/SpawnProcess 전부 구현 완료). `SetTaskWeight`가 Task/스케줄러 개념이지만 유저가 보기엔 "내 프로세스의 우선순위를 바꾼다"는 Process 단위 동작이라 별도 그룹을 신설하지 않고 그룹 0에 자연스럽게 합류한다(`Wait`/`Kill`도 Task가 아니라 Process 단위 개념을 다루면서 그룹 0에 있는 것과 같은 선례).

## 8. 새로 등장하는 용어 (RM-32D06563 §12 규칙 대상)

`Task::weight`/`Task::vruntime`/`Task::cpuTicksUsed`/`TaskVruntimeTraits`/`kEffectiveWeightOf`/`kVruntimeScale`/`SetTaskWeight`(syscall)/`Process::memoryBytesUsed`/`ResourceGroupAccounting::totalMemoryBytesUsed`를 RM-32D06563에 등록 필요(승인 후 반영). `RM-48E1E610` 그룹 0에도 `SetTaskWeight`(call 5) 등록 필요.

## 9. 검증 계획 (착수 시)

1. **vruntime 공정성**: QEMU에서 서로 다른 개수의 CPU-bound 진단 Task(전부 기본 `weight=0`)를 동시에 스폰해, 장시간 실행 후 `cpuTicksUsed`가 서로 근사하게 균등한지 확인(완전히 동일하진 않음 - 선형 삽입 정렬의 근사적 성질 때문에 약간의 편차는 정상).
1-A. **`SetTaskWeight` 차등 확인**: 두 진단 Task를 서로 다른 weight(예: -50 vs +50)로 설정해 동시 실행 후, `cpuTicksUsed` 비율이 대략 `kEffectiveWeightOf(50) : kEffectiveWeightOf(-50)`(150:50 = 3:1) 근처로 나오는지 확인. 범위 밖 값(-101/101)을 넘기면 `ok=false`로 거부되는지도 확인.
1-B. **고정소수점 스케일 정밀도 확인(QU-DA6C52BA 정정)**: `weight > 0`(실효 가중치 > 100)인 Task가 `kVruntimeScale` 없이는(스케일을 일부러 1로 되돌린 디버그 빌드 등) `vruntime`이 전혀 증가하지 않아 다른 모든 Task를 굶기는 회귀를 실제로 재현해, 스케일 적용 버전에서는 그 회귀가 없는지(양의 가중치 Task도 정상적으로 vruntime이 증가하며 순환하는지) 대조 확인 - 이 정정이 실제로 막는 버그가 눈에 보이는 형태로 남도록.
2. **새 Task 굶주림 방지**(§2.3): 이미 오래 실행 중인 Task들이 있는 코어에 새 Task를 스폰했을 때, 그 새 Task가 합리적인 시간 안에 최초로 선택되는지 확인(무한정 뒤로 밀리지 않는지).
3. **RT/Immediate 클래스 무영향**: 기존 `gRtQueues`/`gImmediateQueues` 관련 시나리오(있다면) 전부 무회귀.
4. **ResourceGroup 계정**: 그룹에 가입된 프로세스의 `totalCpuTicks`가 실제 실행 시간과 비례해 증가하는지, 그룹에 없는(루트) 프로세스도 `gRootResourceGroup.accounting`에 정상 집계되는지.
5. QEMU 4개 표준 시나리오 무회귀(스케줄러 핫패스 변경이라 특히 신중히 - `PN-4190BBD3` 항목3이 이미 이 위험을 경고해 둔 그대로).

## 10. 착수 조건

없음 - `Task`/`ResourceGroup`/`OrderedList`(libkcont, 이미 구현) 전부 이미 존재한다. `approved` 전환 후 구현 계획(PN)을 별도 등록해 진행한다(CLAUDE.md 규칙 7) - 스케줄러 핫패스 변경이라 `PN-4190BBD3`과 마찬가지로 신중한 단일 증분으로 진행할 것을 제안한다.
