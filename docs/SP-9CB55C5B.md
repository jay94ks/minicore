# Kill 대상 확장 — 안전한 ProcessId 해석 메커니즘 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-9CB55C5B
  status: review
  updatedAt: 2026-09-17T01:56:57.670Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->


# Kill 대상 확장 — 안전한 ProcessId 해석 메커니즘 설계 제안

QU-764C5624 답변: "임의 프로세스 대상이 필요함 - 설계안 작성해줘."
`Kill`(RM-48E1E610 29번)의 `targetProcessId` 스코프를 "호출자의
직계 자식만"에서 임의 프로세스로 넓히는 설계를 제안한다. 이 문제는
SP-9A6D579F(프로세스 디버깅) §3.2/§8이 이미 "부모-자식 판정에 실제
pid→Process* 안전 해석 인프라가 필요하다"고 지적해 둔 것과 **같은
근본 문제**라, 이 문서가 그 인프라 자체를 설계하고 두 소비자
(`Kill`, `DebugAttach`)가 공유하도록 한다.

## 1. 실측 확인한 현재 상태 — "pid"는 사실 raw 포인터값이다

- `process.h` `SpawnProcessArgs::pid` 문서 주석, `WaitArgs::targetPid`
  문서 주석이 모두 명시: pid는 `reinterpret_cast<int64_t>(Process*)`
  값 그 자체다. 별도의 pid 발급/테이블이 없다 - **포인터가 곧 신원**
  이라는 관례.
- 그래서 지금 `KillHandler::onExec`(process.cpp:791-838)과
  `WaitHandler`는 절대 "유저가 준 int64_t를 바로 `reinterpret_cast
  <Process*>`해서 역참조"하지 않는다 - 대신 호출자의
  `self->children`(커널이 소유한, 크기가 알려진 리스트)를 순회하며
  **포인터 값을 비교**만 한다(process.cpp:819-826). 이 우회로 덕분에
  "유저가 임의 정수를 아무 Process*로 역참조시키는" 취약점이 원천
  차단돼 있다 - **이게 바로 지금 스코프가 "직계 자식"으로 좁은 진짜
  이유**(단순히 소극적 설계가 아니라 안전을 위한 의도적 제약).
- 이 우회로를 유지한 채로 "임의 프로세스"를 지원하려면, 호출자
  기준으로 안전하게 순회 가능한 리스트가 "직계 자식"보다 훨씬 커야
  한다(예: 전체 프로세스 트리) - 그런데 그렇게 확장해도 근본 문제는
  안 풀린다: 트리를 아무리 넓게 순회해도 **결국 "유저가 준 정수값과
  일치하는 Process를 찾는" 방식 자체가 트리 크기에 비례하는 O(n)
  선형 탐색**이라 값싸지 않고, 무엇보다 SP-9A6D579F가 요구하는
  "커널/커널서비스는 트리 관계 무관하게 임의 대상 허용" 예외를
  풀려면 애초에 트리 순회로는 대상에 도달할 수조차 없다(트리 밖에
  있을 수 있으므로).

**결론: 스코프를 임의 프로세스로 넓히려면 "포인터값이 곧 pid"라는
현재 관례 자체를 바꿔, pid를 커널이 발급/해석하는 불투명한 핸들로
바꿔야 한다.** 이게 이 문서의 핵심 제안이다.

## 2. 제안: 세대(generation) 태그 슬롯 테이블

```cpp
// process.h (제안)
using ProcessId = int64_t;  // 이제 포인터가 아니라 커널이 발급하는 불투명 핸들
constexpr ProcessId kInvalidProcessId = -1;

constexpr uint32_t kMaxProcessTableSlots = 4096;  // [열린 결정, §5] 상한 근거는 설계자 확인 필요

struct ProcessTableSlot {
    WeakPtr<Process> proc;   // lock() 실패 = 이미 죽어 반납됨(안전 - 역참조 없이 판정)
    uint32_t generation = 0; // 이 슬롯이 재사용될 때마다 +1 (ABA 방지)
};

// [동시성] 등록/해제는 SpawnProcess/reap 시점에만 일어나 드묾, 조회
// (Kill/DebugAttach)는 훨씬 잦음 - 읽기 다수/쓰기 희소 패턴이라
// QU-68D76FC4가 currentTask()에 도입 예정인 것과 같은 종류의
// read-write-lock이 이 테이블에도 그대로 들어맞을 후보(별도 설계,
// 착수 시 그 결과물 재사용 검토).
ProcessTableSlot gProcessTable[kMaxProcessTableSlots];
```

- **발급(`kAllocateProcessId`)**: `Process` 생성 시(SpawnProcess 성공
  경로) 빈 슬롯을 하나 찾아(freelist 권장 - 선형 탐색 대신) 그
  슬롯의 `generation`을 1 증가시키고 `proc`에 새 `WeakPtr<Process>`를
  채운 뒤, `pid = (int64_t(generation) << 32) | index`로 인코딩해
  돌려준다. `Process` 쪽에 자기 슬롯 인덱스를 역으로 저장해 둬야
  해제 시 O(1)로 슬롯을 찾을 수 있다(`Process::processTableIndex`
  필드 신설 제안).
- **해제**: 좀비가 부모에게 `wait()`로 회수(reap)되는 시점(Process
  구조체 자체가 반납되는 시점 - §1의 좀비 설명 그대로)에 슬롯의
  `proc`을 비운다. `generation`은 건드리지 않는다(다음 재사용 때
  또 +1).
- **안전 해석(`kResolveProcessId`)** - 이 문서가 푸는 핵심 함수:
  ```cpp
  SharedPtr<Process> kResolveProcessId(ProcessId pid) {
      if (pid == kInvalidProcessId) return {};
      const uint32_t index = static_cast<uint32_t>(pid & 0xFFFFFFFF);
      const uint32_t generation = static_cast<uint32_t>(pid >> 32);
      if (index >= kMaxProcessTableSlots) return {};       // 역참조 없이 범위만 확인
      ProcessTableSlot& slot = gProcessTable[index];
      if (slot.generation != generation) return {};         // ABA 방지 - 유저가 옛 pid를 재사용해도 실제 대상은 절대 안 겹침
      return slot.proc.lock();                              // 실패=이미 죽음, 성공해야만 진짜 살아있는 대상
  }
  ```
  유저가 **어떤 int64_t를 넘기든** 이 함수는 인덱스 범위 검사 +
  `WeakPtr::lock()`만으로 끝난다 - `reinterpret_cast<Process*>`를
  단 한 번도 쓰지 않는다(포인터 자체가 애초에 유저에게 노출되지
  않으므로 위조/추측해도 다른 살아있는 프로세스에 우연히 맞을 확률이
  `generation` 불일치로 걸러진다).

## 3. Kill/DebugAttach가 이 위에서 하는 일 (계층 분리)

`kResolveProcessId`는 **신원 해석**만 한다 - "이 pid가 지금 살아있는
어떤 Process를 가리키는가"에만 답하고, "호출자가 그 대상에게 이
연산을 해도 되는가"(권한)는 각 syscall 핸들러가 별도로 판정한다
(SP-9A6D579F §3.2가 이미 이 2단계 분리를 전제로 쓰여 있음 - "판정
순서: (a) KernelService면 즉시 허용 → (b) 아니면 부모 확인 → (c)
PermissionDenied"). `Kill`도 같은 계층 분리를 따른다:

```
KillHandler::onExec:
  1. target = kResolveProcessId(args->targetProcessId)
     -> 실패 시 NotFound (지금과 동일 에러 코드)
  2. [권한 판정 - §4가 미해결로 남기는 부분]
  3. target->raiseSignal(args->signal)  (기존 로직 그대로)
```

## 4. [열린 결정 — 설계자 확인 필요] Kill의 권한 스코프

QU-764C5624 답변("임의 프로세스 대상이 필요함")은 **해석(resolve)**
범위가 임의여야 한다는 요구까지는 확실하지만, **권한(permission)**
스코프까지 완전히 무제한("아무나 아무나 죽일 수 있다")이라는
뜻인지는 명시돼 있지 않다 - CLAUDE.md 규칙 4에 따라 임의로 정하지
않고 이 문서 등록과 함께 QU를 새로 하나 더 올려 확인을 구한다
(아래 옵션 제시, 이 문서의 QU 참고). 참고로 SP-9A6D579F §3.2가
`DebugAttach`에 이미 확정해 둔 모델이 있어 그대로 재사용하면
일관성은 챙길 수 있다:

- **(A) SP-9A6D579F §3.2와 동일한 모델 재사용(권장)**: 기본은
  "호출자가 대상의 조상(직계 부모뿐 아니라 부모의 부모까지 포함해도
  될지는 별도 세부 결정 필요 - §3.2 원문은 "직접 부모"로 한정했음)"
  일 때만 허용 + `ProcessRole::KernelService`/커널 자신은 예외적으로
  무제한. `Kill`과 `DebugAttach`가 같은 정책을 쓰면 코드도
  `kCheckProcessControlPermission(caller, target)` 하나로 공유
  가능(중복 없음, RM-23F4B687 §4 과설계 방지에도 부합).
- **(B) 완전 무제한**: 살아있는 어떤 pid든 알기만 하면(추측/유출로
  얻은 값 포함) Kill 가능. 구현은 가장 단순하지만, pid가 좁은
  32~64비트 정수라 브루트포스 추측 공격 표면이 생긴다(§2의
  generation 태그가 예측을 어렵게는 하지만 암호학적 보장은 아님).
- **(C) (A)의 부모-자손 관계를 "직계 부모"가 아니라 "조상 전체
  (트리상 임의 depth)"로 넓힘** - 임의 대상이 필요한 실제 유스케이스가
  "여러 세대 아래 손자 프로세스를 정리"하는 시나리오라면 이쪽이 더
  맞을 수 있다.

## 5. 그 외 열린 파라미터

- `kMaxProcessTableSlots = 4096`(§2 예시값)은 **추측값** - 시스템
  전체 동시 생존 프로세스 상한으로 적절한지 설계자 확인 필요(같은
  QU에 묶어 확인).
- 테이블 동시성 보호 방식(§2의 read-write-lock 언급)은 이 문서
  범위 밖 - 별도 계획으로 분리 예정(PN-90BD044E가 이미 다루고 있는
  "커널 전역 테이블류 동시성 보호 부재" 계열 문제와 같은 종류라
  그쪽에 실제 구현 시 참고 관계만 걸어 둔다).
- `Wait`(`WaitArgs::targetPid`/`reapedPid`)도 지금 raw 포인터값
  관례를 그대로 쓰고 있어 이 문서의 `ProcessId` 재정의가 적용되면
  같이 영향을 받는다 - 다만 `Wait`은 이미 "직계 자식만"으로 충분한
  의미론이라(POSIX `wait()`도 그렇다) 스코프 확장 필요는 없고,
  **표현 형식만** 새 `ProcessId` 인코딩으로 맞추면 된다(순수 마이그
  레이션, 새 정책 없음).

## 6. 마이그레이션 영향 — 이미 나간 ABI를 바꾼다

`SpawnProcessArgs::pid`/`WaitArgs::targetPid`/`reapedPid`가 이미
구현/커밋돼 유저랜드가 "pid = 포인터값"이라는 전제로 쓰고 있을 수
있다(아직 실제 유저랜드 소비자가 있는지는 미확인 - 착수 전에
`docs git grep`으로 확인 필요). §2의 `ProcessId` 인코딩으로 바꾸면
이 필드들의 **값 형식이 바뀐다**(더 이상 포인터로 역산 불가) - 이건
버그 수정이 아니라 확정된 동작을 바꾸는 것이므로 CLAUDE.md 규칙
11(계획 재검토) 정신에 따라 별도 승인 없이 조용히 바꾸지 않는다.
이 문서가 approved되고 §4의 QU가 해소되면, 실제 구현 착수 전에
"SpawnProcess/Wait의 pid 필드 인코딩 변경" 항목을 PN으로 등록해
그 승인까지 받고 진행한다.

## 7. 요약 — 이 문서가 확정 짓는 것 / 안 짓는 것

- **제안(확정 아님, §4 QU 대기)**: `ProcessId`를 포인터값에서
  세대 태그 슬롯 인덱스로 바꾸는 `kResolveProcessId()` 메커니즘.
- **확정 안 함(§4가 명시적으로 열어 둠)**: `Kill`의 최종 권한 스코프.
- **후속 계획으로 분리**: 테이블 동시성 보호(§5), pid ABI 마이그레이션
  승인(§6) - 둘 다 이 문서 승인 이후 별도 PN으로 등록 예정.

