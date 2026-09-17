# gCurrentTask 크로스코어 접근 보호 — RwSpinlock 설계 스케치 (QU-68D76FC4)

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-9F1DB1D8
  status: approved
  updatedAt: 2026-09-17T02:18:05.726Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->


# gCurrentTask 크로스코어 접근 보호 — RwSpinlock 설계 스케치

QU-68D76FC4 답변: "read-write-lock을 설계하고, 쓰는 동안에만 lock이
잡히도록 currentTask를 수정하는 방안을 찾아봐." PN-5BBD4301(다섯
번째 "onExec()의 Scheduler::currentTask() 오용" 재발) 후속으로
등록된 질의에 대한 첫 스케치 - minicore-f8 세션이 "스케줄러 핫패스라
currentTask()의 실제 호출 패턴/빈도를 잘 아는 쪽이 설계하는 게
맞다"며 이 세션에 먼저 초안을 맡겼다. **비판적 검토를 위해 초안임을
명시하고 등록** - 착수 전 minicore-f8/설계자 확인 필요.

## 0. 먼저 짚어야 할 것 — 이 설계가 푸는 문제 vs 안 푸는 문제

**정직하게 밝혀 둔다**: 이 문서가 제안하는 락은 `gCurrentTask[]`
배열 자체의 **크로스코어 데이터 레이스**를 막는다 - PN-5BBD4301이
발견한 버그(onExec() 안에서 `Scheduler::currentTask()`를 잘못된
문맥에서 호출해 "제출자가 아닌 다른 Task"를 돌려받음)를 **직접
고치지는 않는다**. 그 버그는 락 경합이 아니라 "이 API가 지금 어떤
Task를 가리키는지에 대한 호출부의 착각"이었고, 애초에
`Scheduler::currentTask()`는 항상 유효한(널이거나 다른 Task를
가리키는) 값을 반환했다 - 값 자체가 깨진 적은 없다. 이 문서는 QU가
명시적으로 요청한 "RW-lock 설계"를 그대로 제공하되, 그게 다섯 번째
재발의 근본 수정책은 아님을 §7에서 다시 요약한다(원래 제시했던
린트/공용 헬퍼 방안은 설계자가 채택하지 않았으므로 그 문제 자체는
여전히 열려 있다).

## 1. 실측 확인 — 진짜 레이스는 어디에 있는가

`gCurrentTask[kMaxCores]`(scheduler.cpp:174)의 접근부를 전부
추적했다:

**쓰기(전부 같은 코어 자신만 - 실측 확인)**:
- `onTick()`(:983) - 틱 인터럽트, 그 코어 자신
- `onForcedMigration()`(:1043, :1050) - IPI로 fromCore에서 실행되지만
  `coreIndex = currentCoreIndex()`로 항상 자기 자신의 슬롯만 씀
- `runLoop()`(:1129, :1149) - 그 코어의 디스패치 루프
- `yieldCurrent()`(:1177), `parkCurrent()`(:1225 추정) - 호출자 자신의
  코어

**전부 `cli`로 감싸져 있다** - 즉 "같은 코어 안에서" 이 쓰기와
경쟁할 수 있는 유일한 존재(그 코어 자신의 인터럽트 핸들러)가 이미
차단된 상태에서 일어난다. **같은 슬롯을 다른 코어가 쓰는 경우는
없다** - 코드 전체에 그런 경로가 없음을 확인.

**읽기 - 두 종류로 갈린다**:
1. **같은 코어, 매우 빈번(핫패스)**: `currentTask()`(:1153) - 거의
   모든 syscall 핸들러/AsyncTask onExec()이 간접적으로 이 경로를
   거친다(SP-6BEAE0C1/여러 핸들러). 호출자 자신의 슬롯만 읽는다.
2. **다른 코어, 매우 드묾(v1 기준 사실상 진단/수동 API 전용)**:
   - `taskOnCore()`(:1158) - TLB 샷다운의 Active CPU Mask 스캔
     (PN-D132A1E9) 전용, 자기 문서 주석이 이미 "낡은 값 봐도 무방,
     이 값으로 IPI 보낼지 정도의 휴리스틱에만 쓸 것"이라고 명시.
   - `requestForcedMigration()`(:1000) - 그 자신의 문서 주석이 이미
     "IPI 도착 시점엔 이미 다른 Task로 바뀌어 있을 수 있는 스냅샷"
     이라고 명시하고, `onForcedMigration()`의 `current !=
     gForcedMigrationRequest.target` 방어가 그 낡음을 무해하게
     처리한다(v1은 이 API 자체가 호출부 하나뿐인 수동/진단 경로).
   - **[추가, 2026-09-17, minicore-f8 검토로 발견 - 최초 조사 누락]**
     `kWakeCoreIfIdle(coreIndex)`(:495-498) - `Scheduler::enqueue()`
     (:842-843)가 Push 로드밸런싱으로 `targetCore != coreIndex`(다른
     코어 큐에 밀어 넣은 경우)일 때만 그 `targetCore`의
     `gCurrentTask`를 읽어 idle이면 깨움 IPI를 보낸다 - 호출한 코어
     자신이 아니라 targetCore를 읽는 크로스코어 접근으로, 위 두
     지점과 동일한 패턴. 이 함수 자신의 문서 주석도 이미 "정확한
     판정이 아니지만(idle 진입 직전/직후의 좁은 창) 안전한 근사 -
     false positive 비용이 낮다"고 명시해 둬, 위 두 지점과 같은
     "스탤값 허용" 설계 철학을 공유한다.

**결론(정정)**: 진짜(엄밀한 C++ 메모리 모델 의미의) 데이터 레이스는
"다른 코어가 비동기적으로 갱신 중인 `Task*` 슬롯을 락/원자 연산 없이
평범한 포인터로 읽는" **세 지점**(`taskOnCore`/
`requestForcedMigration`/`kWakeCoreIfIdle`)이다(최초 버전은 두 지점만
찾아 부정확했다 - minicore-f8 검토로 세 번째를 발견, 감사) - 실무적
으로는 이미 스탤값 허용 설계라 지금까지 문제를 일으키지 않았지만,
표준적으로는 미정의 동작이고 컴파일러가 그 값을 레지스터에 캐싱해
두고 다시 안 읽어올 여지가 이론상 있다. **같은 코어 자신의
`currentTask()` 읽기는 애초에 레이스가 아니다** - 한 코어는 한
번에 하나의 명령어 스트림만 실행하므로, 자기 슬롯을 자기가 쓰는
시점과 자기가 읽는 시점은 프로그램 순서로 이미 전순서(total
order)다.

## 2. 제안 — 비대칭 RwSpinlock: 크로스코어 접근만 잠그고, 동일 코어
핫패스는 그대로 락-프리로 둔다

QU 답변의 "쓰는 동안에만 lock이 잡히도록"을 문자 그대로도 만족하고,
동시에 §1이 확인한 "레이스가 실제로 있는 지점만" 보호해 스케줄러
핫패스(`currentTask()`)에 락 오버헤드를 전혀 얹지 않는 절충안을
제안한다:

```cpp
// libkenv/spinlock.h에 추가 제안 - 기존 Spinlock/Atomic<T>과 같은
// 위치, 같은 스타일(컴파일러 내장 원자 빌트인만 사용).
//
// 읽기 다수/쓰기 희소 패턴 전용의 최소 스핀 기반 RW락. **쓰기
// 우선순위(anti-starvation) 없음** - 리더가 계속 몰리면 라이터가
// 무기한 대기할 수 있다. 이 프로젝트에서 첫 소비자로 예정된
// gCurrentTask 크로스코어 접근은 라이터가 압도적으로 드물고(스케줄러
// 디스패치 지점 몇 곳) 리더도 드물어서(TLB 샷다운/강제 이관 진단
// API) 실질적 스타베이션 위험이 없다고 판단해 생략했다(RM-23F4B687
// §4 과설계 방지) - 만약 이후 다른 소비자가 리더 폭주 패턴이면 그때
// 티켓 기반 등으로 재검토.
class RwSpinlock {
public:
    void lockRead() {
        for (;;) {
            uint32_t v = _state.load();
            if (v != kWriteLocked && _state.compareExchange(v, v + 1)) {
                return;
            }
            asm volatile("pause");
        }
    }
    void unlockRead() { _state.fetchSub(1); }

    void lockWrite() {
        uint32_t expected = 0;
        while (!_state.compareExchange(expected, kWriteLocked)) {
            expected = 0;
            asm volatile("pause");
        }
    }
    void unlockWrite() { _state.store(0); }

private:
    static constexpr uint32_t kWriteLocked = 0xFFFFFFFFu;
    Atomic<uint32_t> _state{0};
};

class RwSpinlockReadGuard { /* lockRead/unlockRead RAII, Spinlock Guard와 동일 패턴 */ };
class RwSpinlockWriteGuard { /* lockWrite/unlockWrite RAII */ };
```

적용:

```cpp
// scheduler.cpp
Task* gCurrentTask[kMaxCores] = {};
RwSpinlock gCurrentTaskLock[kMaxCores];  // 슬롯마다 하나 - 코어 간 경합 자체가 없으므로 전역 락 하나로 묶을 이유 없음

// [갱신, 2026-09-17, §7 QU-E847DB03] 설계자 답변이 이 절충을
// 명시적으로 확인해 주지 않아 보수적으로 철회 - currentTask()도
// 예외 없이 읽기 락을 탄다(§1의 "레이스가 아니다" 논증 자체는
// 유효하지만, 확인 안 된 최적화를 임의로 유지하지 않는다).
Task* Scheduler::currentTask() {
    const uint32_t coreIndex = currentCoreIndex();
    RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
    return gCurrentTask[coreIndex];
}

// 크로스코어, 드묾 - 이제 보호됨
Task* Scheduler::taskOnCore(uint32_t coreIndex) {
    RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
    return gCurrentTask[coreIndex];
}

// requestForcedMigration()도 동일하게 gCurrentTaskLock[fromCore] 읽기 락으로 감싼다.
// kWakeCoreIfIdle(targetCore)도 동일하게 gCurrentTaskLock[targetCore] 읽기 락으로 감싼다.

// 각 쓰기 지점(onTick/onForcedMigration/runLoop/yieldCurrent/parkCurrent) -
// 대입 한 줄만 감싼다("쓰는 동안에만" - QU 답변 그대로):
{
    RwSpinlockWriteGuard guard(gCurrentTaskLock[coreIndex]);
    gCurrentTask[coreIndex] = next;  // 또는 nullptr
}
```

## 3. 왜 동일 코어 읽기를 락-프리로 남겨도 안전한가 (핵심 주장)

한 코어는 항상 정확히 하나의 명령어 스트림만 실행한다(이 커널
전체의 기존 불변조건 - `Scheduler::retireTask()` 문서 주석이 이미
같은 논리를 명시적으로 씀). 따라서 코어 C가 자기 자신의
`gCurrentTask[C]`를 쓰는 시점과 코어 C 자신이 그 값을 읽는 시점은
**항상 프로그램 순서(program order)로 전순서**다 - 두 이벤트 사이에
"동시에"라는 개념이 성립하지 않는다(다른 코어가 그 슬롯을 쓰는
경로가 §1에서 확인한 대로 존재하지 않으므로). 락이 막아야 하는
건 "동시에 여러 실행 흐름이 같은 메모리를 건드리는 상황"인데,
`currentTask()`는 그 상황 자체가 발생할 수 없는 접근이다 - 락을
씌우면 코드가 더 안전해지는 게 아니라 스케줄러 핫패스에 순수
오버헤드만 늘어난다(RM-23F4B687 §4).

## 4. 왜 크로스코어 접근엔 락이 실제로 유익한가

`taskOnCore()`/`requestForcedMigration()`은 이미 "낡은 값을 봐도
무방"하다고 문서화돼 있지만, "낡은 값"과 "미정의 동작"은 다르다 -
지금처럼 평범한 `Task*` 읽기/쓰기는 C++ 메모리 모델상 동기화되지
않은 접근이라 엄밀히는 데이터 레이스(표준 위반)다. `RwSpinlock`을
씌우면: (a) 읽기 쪽이 최소한 그 순간 쓰기 중이 아닌 값을 읽는다는
보장이 생기고(반쪽만 쓰인 값을 볼 걱정 자체가 원래도 없었지만 -
8바이트 정렬 포인터는 x86-64에서 원자적으로 읽고 써진다 - 이건
"표준을 지키는 코드"가 되는 이득이지 "지금 실제로 깨졌던 걸
고친다"는 이득은 아니다), (b) 컴파일러가 `gCurrentTask[coreIndex]`
값을 최적화 과정에서 캐싱해 두고 다시 메모리에서 읽어오지 않을
이론적 여지를 없앤다(락 lock/unlock의 획득/해제 시맨틱이 컴파일러
재정렬을 막는 배리어 역할도 함).

## 5. 대안 — 검토했으나 기각

- **`gCurrentTask`를 통째로 `AtomicPtr<Task>[kMaxCores]`로 바꾸고
  락 자체를 없앰**: `Atomic<T>::load()/store()`가 이미 `__ATOMIC_
  ACQUIRE`/`__ATOMIC_RELEASE`라 §4의 이득만 필요하면 이쪽이 더
  가볍다(락보다 원자 연산 하나가 저렴). **그런데 QU 답변이 명시적으로
  "read-write-lock을 설계하고"라고 못박아서, 원자 변수 치환만으로는
  질문이 요구한 산출물(RW-lock 자체)을 안 만든 게 된다** - 이 방안은
  §7 QU에서 "그래도 원자 변수 치환이 충분하면 이쪽이 더 간단한데
  괜찮은지" 확인차 함께 제시한다.
- **모든 접근(같은 코어 포함)을 락으로 통일**: 가장 단순하고 "왜
  같은 코어는 빼지?"라는 질문 자체가 안 생기지만, §3의 근거로 핫패스
  (`currentTask()`, 사실상 모든 syscall onExec()이 거쳐감)에 불필요한
  스핀락 획득/해제 왕복을 추가한다 - 스케줄러 핫패스라는 이 문제의
  전제(peer 세션도 강조)와 정면으로 배치돼 기각했다.

## 6. 구현 파급 범위

- `libkenv/spinlock.h`: `RwSpinlock`/`RwSpinlockReadGuard`/
  `RwSpinlockWriteGuard` 추가.
- `scheduler.cpp`: `gCurrentTaskLock[kMaxCores]` 추가, 쓰기 4곳
  (`onTick`/`onForcedMigration`×2/`runLoop`×2/`yieldCurrent`/
  `parkCurrent`, 정확한 개수는 착수 시 재확인) + 읽기 **4곳**
  (`currentTask`/`taskOnCore`/`requestForcedMigration`/
  `kWakeCoreIfIdle`)에 가드 삽입 - **[갱신, 2026-09-17, §7]**
  `currentTask()`도 예외 없이 포함(같은 코어 락-프리 절충 철회).
- 기존 `cli` 임계구역과 겹치는 쓰기 지점들은 **`cli` 구간 안에서
  락을 잡고 푼다** - `cli`가 이미 그 코어의 인터럽트를 막고 있어
  락 자체가 실제로 경합할 일은 없지만(라이터는 코어당 하나뿐),
  대입 앞뒤로 lock/unlock 두 원자 연산이 추가되는 비용은 있다 -
  yieldCurrent()/onTick() 등의 극도로 예민한 타이밍(§1 인용 주석들이
  경고하는 실측 버그 이력)에 이 추가 비용이 문제가 안 되는지는
  착수 시 QEMU 회귀로 반드시 재확인한다(단순 원자 연산 2회 수준이라
  낙관적으로는 무해할 것으로 예상).

## 7. [해소, 2026-09-17, QU-E847DB03] 답변 및 설계 갱신

**설계자 답변**: "1. lock 카운팅 자체를 lockRead, lockWrite로 해서,
읽기 락이 걸리면 쓰기가 대기하고, 쓰기 락이 걸리면 읽기가
대기하게 만들되, 같은 read끼리는 카운터만 증분되게 만들고, write는
배타적 락으로 유지해. 2. onExec() 안에서 currentTask() 오용문제는
spinlock이 아니라, 락을 소유한 task가 누구인지, core가 누구인지를
함께 관리하여 여러번 호출해도 안전한 구현을 만드는게 맞을것 같네."

**질문1(동일 코어 락-프리 절충) 판정**: 답변은 표준 RW-lock의
일반적 동작(읽기끼리는 카운터만, 쓰기는 배타적)을 재확인했을 뿐,
§3이 제안한 "동일 코어는 애초에 락 자체를 생략" 절충을 명시적으로
승인하지도 거부하지도 않았다 - minicore-f8도 같은 결론(모호함).
**보수적으로 해석해 §3의 절충(예외)을 철회하고, `currentTask()`를
포함한 모든 접근을 예외 없이 `RwSpinlock`을 거치도록 갱신한다**
(아래 §2/§6 갱신) - CLAUDE.md 규칙 4 정신상, 명시적으로 확인 안 된
최적화를 계속 우겨넣지 않는다. §3의 논증 자체(같은 코어 접근은
레이스가 아니다)는 사실로서는 여전히 유효하므로 근거 기록으로만
남겨 둔다 - 만약 이 균일 적용이 실측으로 핫패스에 유의미한
오버헤드를 낸다고 나중에 측정되면, 그 실측 데이터를 들고 다시
질의해 §3 절충의 재도입을 요청할 수 있다(지금은 추측만으로 재질의
하지 않는다).

**질문2(PN-5BBD4301류 재발 방지) 판정**: 답변이 명확하다 - 단순
`RwSpinlock`으로는 부족하고, **"락을 소유한 task/core를 함께
추적해 여러 번 호출해도 안전한" 새 프리미티브**가 필요하다는
뜻으로 읽힌다. 이건 이 문서(§2의 `RwSpinlock`)로 끝나는 게 아니라
**별도의 새 설계 작업**이다 - `RwSpinlock`이 "동시 접근으로부터
값을 보호"하는 것과 별개로, "지금 이 호출이 의미론적으로 올바른
context에서 이뤄지는지"(소유자 추적)까지 검증하는 다른 층위의
문제다. **PN-C536F352로 분리 등록**(task/core 소유자
추적형 접근자 설계) - 이 문서(SP-9F1DB1D8)는 §2의
`RwSpinlock` 메커니즘만 마무리하고, 소유자 추적 설계는 그 계획이
별도로 다룬다. 우선순위는 낮음(PN-CE6A04AB 보안 취약점이 더
급함 - minicore-f8/이 세션 합의, 2026-09-17).

## 8. 이 문서가 확정 짓는 것 / 안 짓는 것

- **확정(§7, QU-E847DB03 답변 반영)**: §2의 `RwSpinlock` 설계 -
  `currentTask()` 포함 모든 gCurrentTask 접근에 예외 없이 적용.
- **실측으로 확정**: §1의 접근부 전수 조사(쓰기 전부 동일 코어,
  읽기는 4곳 - `currentTask`/`taskOnCore`/`requestForcedMigration`/
  `kWakeCoreIfIdle`).
- **이 문서 범위 밖으로 분리**: PN-5BBD4301류(onExec() currentTask()
  오용) 재발 방지는 `RwSpinlock`이 아니라 별도의 "task/core 소유자
  추적형 접근자" 설계가 필요하다는 게 설계자 답변(§7) - **PN-C536F352**
  로 분리 등록. 착수 우선순위는 PN-CE6A04AB(Channel IPC 보안
  취약점)보다 낮다.
- **착수 가능**: §2/§6이 이제 확정됐으므로 실제 구현(PN-D3597800)은
  착수 가능 상태 - 다만 PN-CE6A04AB가 우선.

