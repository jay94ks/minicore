# AsyncTaskCoroYield 재시도 락의 우선순위 역전 라이브락 - drainOnce() 공정성 정책 방향 결정 요청

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-5F0AC0D3
  status: approved
  updatedAt: 2026-09-26T00:13:16.815Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 배경

`PN-F2594E93`(실측 확정)이 발견한 라이브락: `AsyncTaskCoroYield`(원래
"자기 완결적 폴링" 전용으로 설계됨, `PN-A0CEF82D`/`QU-CC8A31F6`)를
`gQuotaCurspaceMutexCore`(`PN-D168A778`)와 `PN-ADA46BF4`가 만들려던
`gBlockBitmapAllocMutexCore`가 "코루틴 안에서 락 재시도"라는 새 용도로
전용했는데, 이 용법이 `AsyncReactor::drainOnce()`의 기존 정책과 만나
영구 라이브락을 일으킨다.

**정확한 메커니즘**(코드로 확정, 디스크 I/O 없이 순수 재현 완료):
- `AsyncTaskCoroYield::await_suspend()`(`async_task.cpp:986-991`)는
  항상 `AsyncReactor::submitCompletion(self, preemptive=true)`로
  자신을 선점 큐에 재제출한다.
- `AsyncReactor::drainOnce()`(`async_task.cpp:727-731`)는 선점
  큐가 완전히 빌 때까지 일반 큐를 절대 보지 않는다(무조건적
  우선순위) - 이 정책은 `PN-4FA5F13B`(다른 Ready Task가 계속 있으면
  폴링이 idle 분기에 못 도달해 무기한 지연되는 굶주림 버그)를 막기
  위해 의도적으로 도입됐다.
- 락 보유자가 임계구역 안에서 다른 `AsyncTask`(예: 디스크 I/O)의
  완료를 `co_await`로 기다리며 suspend되면, 그 I/O는 **일반 큐**에
  들어간다. 그 사이 락을 놓친 대기자가 `tryAcquire`+`AsyncTaskCoroYield`
  로 재시도하면 **선점 큐**에 들어간다. 다음 `drainOnce()`는 선점
  큐 우선 규칙 때문에 대기자를 또 뽑고, 대기자는 락을 여전히 못
  잡으니 다시 선점 큐로 - 이 사이클이 무한 반복돼 보유자의 I/O가
  영원히 드레인되지 않는다(그래서 락도 영원히 안 풀린다).

## 확정된 사실 vs 아직 정해지지 않은 것

이 티켓은 "버그가 있다/없다"를 묻는 게 아니라(이미 실측으로 100%
재현 확정, `PN-F2594E93` 참고), **어떤 방식으로 고칠지**를 묻는다 -
CLAUDE.md 규칙4에 따라 이 커널의 스케줄러 공정성 정책 자체를 바꾸는
설계 결정이라 임의로 정하지 않는다.

## 후보 방향 3가지

**(a) 선점 큐 소비 상한제**: `drainOnce()`가 한 번의 "드레인 사이클"
동안 선점 큐에서 최대 N개까지만 연속으로 처리한 뒤, 선점 큐에
남은 게 있어도 일반 큐를 한 번은 보게 강제한다. 장점: 기존
`AsyncTaskCoroYield` 사용처(예: `ahci.cpp`의 자기 완결적 폴링)의
"즉시 재확인" 특성을 크게 해치지 않음. 단점: N을 얼마로 잡을지가
경험적 튜닝이 되고, 이론적으로는 여전히 극단적인 경우 지연이 커질
수 있음.

**(b) 재시도 카운트 기반 강등**: `AsyncTaskCoroYield`가 자신이 몇
번째 연속 재시도인지 세어(예: `AsyncTask` 구조체에 필드 추가 또는
handler 쪽에서 관리), 일정 횟수(예: 8회)를 넘으면 그다음 재시도는
`preemptive=false`로 제출해 일반 큐로 "강등"시킨다. 장점: 정상적인
(자기 완결적) 폴링 용도는 전혀 영향 없음 - 그런 용도는 보통 몇 번
안에 끝남. 단점: `AsyncTaskCoroYield` 자체의 계약을 바꾸는 것이라
기존 모든 사용처(자기 완결적 폴링 포함)의 동작이 미묘하게 바뀜 -
회귀 검증 범위가 넓어짐.

**(c) 락 재시도 전용 새 프리미티브 신설**: `AsyncTaskCoroYield`를
"락 재시도" 용도로는 아예 쓰지 않고, `WaitInterruptHandler`가 이미
쓰는 패턴(`kernel::SuspendAlways` + 외부에서 명시적으로
`popFront()`+`submitCompletion()`으로 깨우는 것)을 본떠 "이 락이
`release()`될 때 대기자를 실제로 깨우는" 새 wait-queue 기반 프리미티브를
설계한다. 장점: 가장 근본적인 해결 - 재시도/폴링 자체가 없어지므로
이 라이브락 계열이 구조적으로 성립 안 함, 스핀 낭비도 없음. 단점:
새 자료구조(대기자 큐) 설계+구현이 필요해 셋 중 가장 큰 작업량 -
`AsyncTask` 구조체에 "이 락을 기다리는 중"이라는 상태를 추가하는
방식이 될 가능성이 높음(기존 `waitingAsyncTask` 필드와 유사한 패턴
재사용 검토 여지 있음).

## 영향받는 기존 코드

- `gQuotaCurspaceMutexCore`(`ext4_driver.cpp`, `PN-D168A778`) -
  이미 존재하는 유일한 실사용처, 지금까지 무경합 조건에서만
  검증됨(재검증 필요).
- `PN-ADA46BF4`(Ext4Driver 블록 비트맵 동시성 버그) - 이 결정을
  기다리며 블로킹 중, 새 락 `gBlockBitmapAllocMutexCore`가 아직
  미착수.
- `ahci.cpp`의 `AhciCommandHandler` 등 기존 `AsyncTaskCoroYield`
  "자기 완결적 폴링" 사용처 - 어느 방향을 택하든 회귀 없는지 확인
  필요(특히 (b)).

## 참고
- `PN-F2594E93` - 이 문제의 실측 확정/근본 원인 분석 전체.
- `PN-ADA46BF4` - 이 결정을 기다리는 구체적 수정 작업(ext4 블록 비트맵).
- `PN-D168A778` - 기존 `gQuotaCurspaceMutexCore`도 같은 위험에
  노출됨.
- `PN-A0CEF82D`/`QU-CC8A31F6` - `AsyncTaskCoroYield` 원 설계 의도.
- `PN-4FA5F13B` - `drainOnce()`의 무조건적 선점 큐 우선 정책이
  원래 막으려 했던 굶주림 버그.

