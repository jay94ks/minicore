# Running Task 강제 이관(선점형 로드밸런싱) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-ECC59BAE
  status: review
  updatedAt: 2026-09-16T02:52:59.209Z
  갱신: node scripts/export-cnw-docs.mjs
-->
# Running Task 강제 이관(선점형 로드밸런싱) — 설계 제안

설계자 지시(2026-09-15, PN-BAA0B460) - "Running Task의 강제 이관은
반드시 필요해. 별도로 설계안 작성해줘." SP-9525C4C0(Push/Pull
로드밸런싱, approved)이 §1에서 v1 범위 밖으로 명시적으로 미룬
"실행 중(Running) Task를 다른 코어로 강제로 옮기는" 선점형 이관을
다룬다 - SP-9525C4C0 §1/§5.3/§6 항목1을 전제로 삼는다.

## 1. 이 문서가 푸는 문제와 안 푸는 문제

SP-9525C4C0의 Push/Pull은 **Ready 큐에 대기 중인 Task만** 옮긴다
(§1) - 이미 어느 코어에서 `Running`인 Task는 절대 건드리지 않는다.
그 코어가 다음에 `pickNext()`를 다시 확인할 때까지(스케줄러 틱에
의한 자연스러운 라운드로빈 전환, 100Hz) 기다려야만 그 Task가
"Ready 큐 대기 중" 상태로 바뀌어 로드밸런싱 대상이 될 수 있다.

**이 문서가 푸는 문제**: 그 자연스러운 전환을 기다리지 않고, 지금
이 순간 다른 코어에서 실제로 실행 중인 Task를 그 자리에서 강제로
끄집어내 다른 코어로 옮긴다(선점형).

**이 문서가 안 푸는 문제(범위 밖, 별도 DC 필요시 후속)**: "언제
이 강제 이관을 발동해야 하는가"의 정확한 트리거 조건(임계치/판단
기준)은 §5에서 후보만 제시하고 확정하지 않는다 - CLAUDE.md 규칙 4에
따라 이 부분은 이 문서 승인과 별개로 설계자 확인을 구한다(§5 말미
질의 참고).

## 2. 왜 "그냥 IPI로 Task를 옮기기"가 생각보다 단순한가

언뜻 "실행 중인 걸 강제로 옮긴다"는 게 완전히 새로운 메커니즘이
필요할 것 같지만, 실제로는 **이미 존재하는 `onTick()`의 Task-to-Task
전환 로직(scheduler.cpp)을 거의 그대로 재사용**할 수 있다 - `onTick()`
자체가 이미 "지금 Running인 Task를 그 자리에서 다른 Task로 강제
전환"하는 코드이기 때문이다(매 100Hz 틱마다 이미 일어나는 일). 유일한
차이는 전환된 뒤 원래 Task를 **어느 코어의 큐에 다시 넣는가**뿐이다:

```cpp
// scheduler.cpp, 기존 onTick() 발췌(참고용, 실제로는 아래 §3의
// 새 IPI 핸들러가 이와 거의 동일한 코드를 쓰되 targetCore만 다르다)
Task* next = pickNext(coreIndex);
if (!next) { return; }
if (current->state != TaskState::Zombie) {
    enqueue(coreIndex, current);  // <- 기존: 항상 "같은" 코어에 재삽입
}
gCurrentTask[coreIndex] = next;
next->state = TaskState::Running;
kSyncRsp0ForDispatch(next);
kSyncCr3(next);
kSyncFpu(next, coreIndex);
kContextSwitch(&current->savedRsp, next->savedRsp);
```

`kContextSwitch`가 저장/복원하는 것은 System V 콜리세이브
레지스터+RFLAGS뿐이다(`current->savedRsp`) - CR3/RSP0/FPU는 여기서
전혀 건드리지 않는다. SP-9525C4C0 §5.1/§5.2가 이미 증명했듯
CR3(`kSyncCr3`)/RSP0(`kSyncRsp0ForDispatch`)는 Task 소유 데이터만
참조해 **"이 Task가 다음에 어느 코어에서 재개되든" 상관없이 그
재개 지점(§3.2의 세 훅)에서 다시 올바르게 동기화된다** - 이 성질은
"Ready 큐 대기 중 이관"이든 "Running 상태에서 강제로 끌려나옴"이든
완전히 동일하게 성립한다(Task가 어떤 상태였는지는 CR3/RSP0의
정확성과 무관 - 둘 다 순수하게 Task 자신의 필드값만 본다).

**즉 이 설계의 핵심은 "새 컨텍스트 스위칭 메커니즘"이 아니라
"기존 onTick() 전환 로직에서 `enqueue(coreIndex, ...)`의 대상
코어를 이번만 다르게 지정하는 것" + "그 전환을 특정 코어에 지금
당장 강제로 유발하는 IPI"다.**

## 3. 메커니즘

### 3.1 새 IPI 벡터 - `kForcedMigrationVector`

TLB 샷다운(`tlb_shootdown.cpp`)/로드밸런싱 idle 기상(SP-9525C4C0 §4)
과 동일한 패턴 재사용 - 새 동시성 프리미티브 없음, `Lapic::
sendFixedIpi(destApicId, vector)` 그대로.

RM-28225668(인터럽트 벡터 목록)에 `0xE2`로 신규 배정(다음 미사용
번호, 규칙대로).

```cpp
// scheduler.h/scheduler.cpp
constexpr uint8_t kForcedMigrationVector = 0xE2;  // RM-28225668

// 코어별로 "지금 이 코어에서 강제로 끌어낼 대상 Task와, 어디로
// 보낼지"를 IPI 페이로드 없이(x86 고정 벡터 IPI는 인자를 못 실음)
// 전달하기 위한 요청 슬롯 - tlb_shootdown.cpp의 g_tlbShootdownRequest
// 단일 슬롯과 동일한 관례(그 문서가 이미 남겨 둔 "동시 호출자가
// 여럿이면 슬롯을 늘리거나 직렬화 락이 필요하다"는 경고도 그대로
// 유효 - PN-D132A1E9가 그 재검토를 추적 중이니 이 설계도 같은
// 전제(현재는 단일 직렬화 호출 경로만 있음)를 공유한다).
struct ForcedMigrationRequest {
    Task* target = nullptr;     // 끌어낼 Task(반드시 이 요청을 받는
                                 // 코어에서 지금 Running이어야 함)
    uint32_t targetCore = 0;    // target을 재삽입할 코어
};
ForcedMigrationRequest gForcedMigrationRequest;  // 파일 스코프, 단일 슬롯
```

### 3.2 요청 발행 - 어느 코어에서나 호출 가능

**[비판적 재검토, 2026-09-16 - 문서 리뷰 세션 발견]** 아래 `gForcedMigrationRequest`
단일 슬롯이 안전하다는 전제("호출부가 이미 단일 직렬화 경로")는 아직
확정되지 않은 §5의 트리거 정책 설계에 기대고 있다 - §5의 세 후보 중
(A)/(B)가 채택되면 그 판단 로직이 정확히 몇 개의 호출 경로에서(예:
매 코어의 `onTick()`이 각자 독립적으로 판단하는 구조라면 여러 코어가
동시에 `requestForcedMigration()`을 부를 수 있음) 실행될지가 §5
확정 전까지는 불명확하다. PN-D132A1E9(유저 영역 TLB 샷다운도 같은
단일 슬롯 패턴에 같은 경고를 이미 남겨 둠)와 마찬가지로, **이 "단일
직렬화 경로" 가정은 §5가 확정되고 실제 호출 지점이 정해진 뒤 반드시
재검증해야 한다** - 여러 코어가 동시에 이 함수를 호출할 수 있는
구조로 확정되면 슬롯을 늘리거나(PN-D132A1E9 (A)안과 동일 패턴) 요청
자체를 직렬화하는 락이 필요하다.

```cpp
// Scheduler::requestForcedMigration(fromCore, targetCore) -
// fromCore가 지금 실행 중인 Task를 targetCore로 강제 이관한다.
// 호출부(§5의 로드밸런서 판단 로직)가 어느 코어에서 실행되든
// 무방하다(IPI는 코어 간 비동기 요청이므로).
void Scheduler::requestForcedMigration(uint32_t fromCore, uint32_t targetCore) {
    // tlb_shootdown.cpp와 동일한 이유로 요청 슬롯 채우기 자체는
    // 직렬화가 필요하다(PN-D132A1E9 경고 동일 적용) - v1은 호출부가
    // 이미 단일 직렬화 경로(§5)라 별도 락 없이 그대로 채운다. **단,
    // 위 [비판적 재검토] 참고 - §5 확정 전까지 이 전제는 가정일 뿐**.
    gForcedMigrationRequest.target = gCurrentTask[fromCore];  // 요청
    // 시점에 스냅샷 - IPI 도착 시점에 이미 다른 Task로 바뀌어 있을
    // 수 있다(§3.3에서 이 경쟁을 무해하게 처리).
    gForcedMigrationRequest.targetCore = targetCore;
    Lapic::sendFixedIpi(Acpi::cpuApicId(fromCore), kForcedMigrationVector);
}
```

### 3.3 IPI 핸들러 - `fromCore` 자신에서 실행

```cpp
// idt.cpp에 kForcedMigrationVector로 등록하는 ISR이 이 함수를 부른다
// (EOI는 공통 ISR 스텁이 처리 - tlb_shootdown.cpp와 동일 관례).
extern "C" void kForcedMigrationIsr() {
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    Task* current = gCurrentTask[coreIndex];

    // 경쟁 방어: IPI가 도착했을 때 이미 이 코어가 idle이거나(current
    // == nullptr) 요청 시점과 다른 Task를 실행 중이면(그 사이 이
    // Task가 스스로 끝났거나 블로킹돼 자연스럽게 전환됐을 수 있음)
    // 이 요청은 그냥 무해하게 버린다 - 목표는 "가능하면 지금 옮긴다"
    // 지 "반드시 옮긴다"가 아니다(근사적 로드밸런싱, SP-9525C4C0의
    // approxLength()와 같은 정신).
    if (!current || current != gForcedMigrationRequest.target) {
        return;
    }
    const uint32_t targetCore = gForcedMigrationRequest.targetCore;

    Task* next = pickNext(coreIndex);  // 이 코어의 다음 Task(없으면
                                        // idle로 - runLoop()과 동일)
    // FPU 강제 반납(§4) - kContextSwitch 전에 반드시 먼저.
    kEvictFpuBeforeMigration(current, coreIndex);

    if (current->state != TaskState::Zombie) {
        enqueue(targetCore, current);  // <- onTick()과 유일하게 다른
                                        // 한 줄: 같은 코어가 아니라
                                        // targetCore에 재삽입.
    }
    if (!next) {
        // 이 코어에 대신 돌릴 다른 Task가 없다 - runLoop()의 idle
        // 분기로 자연스럽게 떨어지도록 gCurrentTask만 비운다(진짜
        // 컨텍스트 스위치는 runLoop()이 다음 루프에서 pickNext()==
        // nullptr을 보고 idle 처리한다 - 이 ISR 자신은 인터럽트
        // 컨텍스트라 여기서 직접 hlt하지 않는다, onTick()의 idle
        // 분기 "return"과 동일한 원칙).
        gCurrentTask[coreIndex] = nullptr;
        // current 자신의 kContextSwitch는 필요하다 - 원래 실행
        // 흐름(이 인터럽트가 끼어든 지점)으로 다시는 돌아오지 않고
        // idle 스택으로 넘어가야 하기 때문이다. runLoop()의 idle
        // 진입과 동일한 대상(gIdleSavedRsp[coreIndex])으로 전환한다.
        kContextSwitch(&current->savedRsp, gIdleSavedRsp[coreIndex]);
        return;  // 이 코어가 나중에 current를 다시 고르면 이 return
                 // 다음(ISR 반환 -> iretq)으로 재개된다 - 그 시점은
                 // 이미 targetCore에서일 것이므로 이 return 자체는
                 // 실행되지 않고 그 코어의 디스패치 훅에서 대신
                 // 재개된다(§2의 핵심 논거와 동일).
    }
    gCurrentTask[coreIndex] = next;
    next->state = TaskState::Running;
    kSyncRsp0ForDispatch(next);
    kSyncCr3(next);
    kSyncFpu(next, coreIndex);
    kContextSwitch(&current->savedRsp, next->savedRsp);
}
```

**인터럽트 컨텍스트에서 `kContextSwitch`를 불러도 되는가**: 된다 -
`onTick()`이 이미 정확히 같은 일(인터럽트 핸들러 안에서
`kContextSwitch`로 다른 Task의 스택으로 전환)을 하고 있다(scheduler.cpp
595행) - 이 설계는 그 확립된 패턴을 그대로 재사용할 뿐 새로운
안전성 논증이 필요 없다.

## 4. FPU 재검증 - SP-9525C4C0 §5.3보다 오히려 더 단순

SP-9525C4C0 §5.3은 "이관을 결정하는 코어"와 "`fxsave`를 실제로
실행해야 하는 코어(`fromCore` 자신)"가 다를 수 있어 IPI 왕복이
필요하다는 게 어려움의 핵심이었다. **이 설계는 그 문제가 애초에
없다** - 강제 이관 메커니즘 자체가 이미 `fromCore`에게 보내는
IPI이므로, `kEvictFpuBeforeMigration()`을 그 IPI 핸들러(§3.3) 안에서
직접 호출하기만 하면 자동으로 올바른 코어에서 실행된다:

```cpp
// SP-9525C4C0 §5.3의 함수를 그대로 재사용(신규 함수 아님)
void kEvictFpuBeforeMigration(Task* task, uint32_t fromCore) {
    if (gFpuOwner[fromCore] == task) {
        asm volatile("fxsave (%0)" : : "r"(task->fpuState) : "memory");
        gFpuOwner[fromCore] = nullptr;
    }
}
```

**왜 안전한가(Ready 큐 이관보다 오히려 더 명확)**: §3.3의 IPI 핸들러가
실행되는 바로 그 순간, `current`(이관 대상)는 **아직 실제로
`Running`인 채로 인터럽트당한 것**이다 - 즉 이 Task가 정말 이 코어의
FPU 하드웨어 소유자라면(`gFpuOwner[fromCore] == current`), 그 레지스터
안의 값은 지금 이 순간까지 이 Task가 실제로 쓰던 살아있는 값이고,
그걸 이 코어 자신이(다른 코어가 끼어들 수 없는 이 ISR 실행 중에)
그대로 `fxsave`하면 된다 - SP-9525C4C0 §5.3처럼 "실용적 절충안(이번엔
이관 보류)"으로 회피할 필요조차 없다. 정확히 이 순간 이 코어에서
FPU를 건드리는 다른 코드가 동시에 실행될 수 없으므로(단일 코어,
인터럽트 컨텍스트) 경쟁도 없다.

## 5. 트리거 조건 - 아직 미확정, 설계자 확인 필요

**이 절만 확정되지 않은 채로 문서를 승인 요청한다** - 메커니즘(§2-4)
은 기존 코드 재사용만으로 충분히 안전하게 증명되지만, "언제
발동할지"는 순수한 정책 결정이라 이 세션이 임의로 정하지 않는다.

### 5.1 왜 어려운가 - "Running Task를 옮기는 것 자체는 일을 줄이지
않는다"

Ready 큐 이관(SP-9525C4C0)은 "밀린 일감을 한가한 코어로 옮긴다"는
직관이 명확하다. Running Task 강제 이관은 다르다 - 그 Task는 어차피
어딘가에서 실행돼야 하므로, 단순히 "옮기는 것" 자체가 전체 처리량을
늘리지 않는다. 이게 실제로 도움이 되는 시나리오는 좁게 한정된다:

- 코어 A가 **RT 클래스** Task를 오래 돌리고 있어(RT는 우선순위상
  Normal에게 밀리지 않으므로 `onTick()`의 자연스러운 라운드로빈이
  이 상황을 절대 못 바꾼다 - SP-9525C4C0 §6 항목4가 RT를 로드밸런싱
  "대상"에서 제외한 것과는 별개로, RT가 **다른 Normal Task들을
  코어 A에서 굶기는** 상황), 코어 A의 Ready 큐에 밀린 Normal Task가
  쌓여만 가는데 다른 코어(B)는 한가한 경우 - 이때 코어 A의 RT
  Task **자신**을 코어 B로 옮기면 코어 A가 그 밀린 Normal Task들을
  돌릴 수 있게 된다. **이 시나리오가 유력해 보이지만, RT Task를
  로드밸런싱 대상으로 다루는 것이 SP-9525C4C0 §6 항목4("RT/Immediate
  제외")와 정면으로 배치되는지 확인이 필요하다.**
- `gPreemptDisableCount[coreIndex] > 0`인 긴 구간(Slab 매거진 조작
  등) 동안은 `onTick()` 자체가 조기 반환하므로(scheduler.cpp
  563-565행) 그 구간이 비정상적으로 길어지는 버그가 있다면 이
  메커니즘으로도 구제할 수 없다(그 구간 자체가 인터럽트에도 응답은
  하되 선점만 안 하는 것 - `PreemptDisableCount`는 진짜 `cli`가
  아니므로 IPI ISR 자체는 여전히 실행되지만, 그 ISR이 또 다른
  선점을 시도하는 건 같은 이유로 위험할 수 있다 - **확인 필요**).

### 5.2 후보 트리거 정책

(A) **RT Task 코어 독점 감지**: 어떤 코어에서 같은 Task(TaskClass::
    RealTime)가 N틱 이상 연속으로 `Running`이고, 그 코어의 Normal
    큐 길이가 0보다 크며, 다른 코어의 Normal 큐가 비어 있으면(또는
    idle이면) 그 RT Task를 그 idle/한가한 코어로 강제 이관한다.
(B) **일반 "코어 잔류 시간" 감지**: RT 여부와 무관하게, 같은 Task가
    한 코어에서 M틱 이상 연속 Running이면서 다른 코어의 큐 불균형이
    임계치를 넘으면 이관 후보로 삼는다(affinityMask 제약은 여전히
    존중).
(C) **아예 자동 트리거를 두지 않고 수동/진단 API로만 노출**: 지금
    v1엔 이 상황을 실측할 만한 워크로드 자체가 없다(RT Task를 실제로
    쓰는 코드가 아직 없음, 유저 프로세스도 사실상 init 하나뿐) -
    메커니즘(§2-4)만 구현해 두고, 실제 자동 발동 정책은 실측
    가능한 워크로드가 생긴 뒤(§6 참고) 재검토한다.

**제 판단(권장, 확정 아님)**: (C)를 v1으로 제안한다 - 메커니즘
자체는 지금 확정/구현하되(다음에 필요해질 때 바로 쓸 수 있도록),
자동 발동 조건은 실측 근거가 전혀 없는 상태에서 숫자(N/M틱, 임계치)
를 정하는 게 RM-23F4B687 §4 취지(추측성 숫자값 확정 지양)에 어긋난다고
판단했다. 확인 부탁드립니다 - (A)/(B)/(C) 중 어느 방향으로 갈지,
그리고 RT Task를 이 메커니즘의 실제 대상으로 삼아도 되는지(§5.1의
배치 우려)를 포함해서요.

## 6. 검증 계획 (착수 시)

- `gEvictFpuBeforeMigration`가 실제로 `Running` 상태의 살아있는 FPU
  값을 손실 없이 옮기는지 - 코어 A에서 부동소수점 계산 중인 임시
  Task를 강제로 코어 B로 이관하고, 계속 계산해 결과가 이관 전/후로
  이어지는지 실측(임시 스캐폴딩, PN-C46DF296/PN-645CF608과 동일한
  검증 관례).
- IPI 도착 시점 경쟁(§3.3의 `current != target` 방어)이 실제로
  발동하는 드문 케이스(요청 직후 그 Task가 스스로 끝나거나 블로킹)를
  인위적으로 유발해 안전하게 무시되는지.
- 이관된 Task가 targetCore에서 정상적으로 이어서 실행되는지(CR3/
  RSP0가 §2의 논거대로 재동기화되는지) - QEMU SMP4 시나리오.

## 선행 조건

- SP-9525C4C0(Push/Pull 로드밸런싱) - approved, §5.3의
  `kEvictFpuBeforeMigration`을 그대로 재사용.
- RM-28225668(인터럽트 벡터 목록) - `0xE2` 신규 배정 필요(이 문서
  승인 시 함께 갱신).
- `tlb_shootdown.cpp`(완료) - §3의 IPI 패턴 선례.
