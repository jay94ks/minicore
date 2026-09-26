# MapleTree v3 — store()/erase()의 인접 2-리프 spanning 지원 — 설계 제안 (SP-2AAD7C8D §3 후속, PN-2EA94B1A/PN-38D17292 known limitation 3 해소)

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-5D62DCCF
  status: review
  updatedAt: 2026-09-26T05:09:33.760Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 0. 배경

`PN-38D17292`(MapleTree v2 - 멀티레벨 노드 분할/병합, completed)가
의도적으로 v2 범위 밖으로 미룬 "known limitation 3"을 해소하는
설계다: 하나의 `store()`/`erase()` 요청 범위가 자식 리프 하나의
경계를 넘으면 `insertIntoInternal()`이 그냥 실패한다
(`minicore/libs/libkenv/maple_tree.h:121-129`).

**실제 트리거(2026-09-26, RM-F2DAFF66 §1-X/§1-Y)**: `Mmap`/`Munmap`/
`Brk` syscall의 부팅 배선 누락을 고치던 중, 이 커널 최초로 실제
ring3 `Brk()` 힙 성장을 실측했다 - 이미 존재하는 평범한 프로세스
(`minicore/init`)에서 힙을 4096바이트만 키우려 해도
`resizeAnonymousRegion()`이 실패했고, 근본 원인이 정확히 이 known
limitation 3이었다. 이전까지는 이 한계를 "프로세스당 최대 VMA
개수" 문제로만 이해했었는데, 실측으로 **개수와 무관하게 노드 경계
하나만 넘어도 실패**한다는 것이 드러나 영향 범위가 처음 알려진
것보다 넓다.

## 1. 현재 구조 (코드 확인 완료, `maple_tree.h`)

- 트리는 항상 `MapleArangeNode`(10슬롯, `kMapleArangeSlotCount`) -
  리프/내부 구분은 `leaf` bool 필드.
- `insertIntoLeaf(leaf, lower, upper, start, end, value, &outOk)` -
  `leaf`가 `[lower,upper]`를 담당한다는 전제로 `decomposeNode()`로
  Span 배열을 얻고, `[start,end]`를 끼워 넣은 새 Span 배열을
  `rebuildNode()`로 다시 쓴다. 슬롯이 넘치면 두 개로 쪼개
  `SplitResult{split=true, separatorKey, sibling}`을 반환.
- `insertIntoInternal(internal, ...)` - `start`를 담당하는 자식
  하나를 찾아 재귀 - **`[start,end]`가 그 자식 하나의 경계를 넘으면
  즉시 `*outOk=false`**(이 SP가 없애려는 바로 그 분기).
- `erase()`는 이미 부분 겹침을 지원(겹치는 기존 엔트리를 자르고
  같은 value로 양쪽에 남김) - 노드 경계를 넘는 erase가 실제로도
  실패하는지는 아래 §4에서 별도로 확인.

## 2. 설계 - "2-리프 spanning" (Linux `mas_wr_spanning_store`의 축소판)

### 2.1 스코프를 의도적으로 좁힌다

Linux 원본의 완전한 spanning store(임의 개수의 리프에 걸친 갱신 +
필요하면 부모까지 재귀적으로 재분배)는 이 프로젝트 규모에는 과하다.
실사용 트리거(brk 성장, 인접 VMA로의 확장)는 전부 **정확히 두 개의
인접 형제 리프**에 걸치는 경우뿐이다 - 한 `store()` 요청의 크기가
리프 하나가 담당하는 가상주소 범위(실측상 GiB급)를 통째로 넘는
것은 현실적인 시나리오가 아니다. 그래서 v3은:

- **정확히 2개의 인접 리프에 걸치는 경우만 지원**한다.
- **3개 이상에 걸치는 요청은 여전히 `outOk=false`로 실패**시키고
  명확히 문서화한다(RM-23F4B687 §4 - 필요한 만큼만, 실사용처 없는
  일반화는 하지 않음). 필요해지면 그때 v4로 확장.

### 2.2 절차 (`insertIntoInternal`에 새 분기)

1. **탐지**: `start`가 속한 자식 인덱스 `i`와 `end`가 속한 자식
   인덱스 `j`를 각각 계산한다(기존에도 `start`용 인덱스는 이미
   계산하고 있으므로, `end`용 계산만 추가).
   - `i == j`면 기존 경로 그대로(변경 없음).
   - `j == i+1`(정확히 인접 형제 하나만 더)이면 아래 spanning 절차.
   - `j > i+1`이면 기존과 동일하게 `outOk=false`.
2. **합치기**: 자식 `i`와 `i+1`을 각각 `decomposeNode()`로 Span
   배열로 풀고, 두 배열을 하나로 이어붙인다(경계에서 `[start,end]`
   가 걸치는 부분은 하나의 새 Span으로 대체 - `insertIntoLeaf()`가
   이미 하는 "기존 Span 배열에 새 Span을 끼워 넣는" 로직을 그대로
   재사용, 자식이 2개라는 점만 다름).
3. **재구성**:
   - 합친 Span 개수가 `kMapleArangeSlotCount` 이하면 **리프 하나로
     합쳐진다** - `rebuildNode()`로 자식 `i`를 그 내용으로 다시
     쓰고, 자식 `i+1`은 `freeSubtree()`로 반납, 부모(`internal`)
     에서 슬롯 하나가 사라진다(`usedSlotCount--`, 그 뒤 슬롯들을
     한 칸씩 앞으로 당김 - 이미 `erase()`가 겪는 "슬롯 제거" 패턴과
     동일).
   - 넘치면 **리프 두 개로 재분배**한다(기존 `rebuildNode`+분할
     로직을 그대로 재사용 - 이미 "10슬롯 초과 시 분할"을 하므로
     이번엔 "20슬롯급 합친 배열을 2개로 재분배"로 일반화만 하면
     됨). 부모 슬롯 개수는 그대로(2개였다가 다시 2개).
4. **부모 자신의 분할 전파**: 위 두 경우 다 부모(`internal`)의
   `usedSlotCount`가 줄거나 그대로일 뿐 **늘지는 않으므로**, 부모
   자신이 새로 꽉 차서 분할돼야 하는 경우는 생기지 않는다(중요한
   단순화 - Linux 원본의 재귀적 부모 재분배가 필요 없는 이유).
   슬롯이 줄어드는 경우(§2.2 첫 갈래)만 반영하면 된다.

### 2.3 erase()도 같은 원칙으로 확인

`erase()`가 두 리프에 걸치는 경우는 사실 store()보다 단순할 수
있다 - 각 리프에서 독립적으로 겹치는 부분만 지우면 되고(이미 부분
겹침 지원), 리프를 하나로 합칠 필요가 없다(오히려 v2의 기존 한계 1
"erase 시 병합 없음"과 같은 선상). **착수 세션이 실제로
`insertIntoInternal`의 erase 경로도 §3 처럼 경계 검사로 막혀 있는지
코드로 재확인할 것** - 이 설계 문서 작성 시점엔 store() 경로만
확실히 확인했다(CLAUDE.md 규칙4 - 확인 안 된 것을 확정으로 적지
않음).

## 3. 검토했으나 채택하지 않은 대안

- **완전 일반화(N개 리프 spanning + 재귀적 부모 재분배)**: Linux
  원본 `mas_store()`/`mas_spanning_rebalance()`의 복잡도를 그대로
  가져오는 셋이다. 실제 트리거가 "인접 리프 2개"뿐인 지금 시점에는
  검증 비용 대비 이득이 낮다고 판단 - RM-23F4B687 §4 원칙.
- **VMA 개수 상한을 낮춰 회피**(예: 인접 확장을 애초에 막음): 이건
  근본 문제(트리 경계 자체의 한계)를 해결하지 않고 증상만 가리는
  방식이라 채택하지 않음.

## 4. 검증 계획 (착수 세션)

1. 인접 두 리프에 걸친 `store()`(예: `Brk()` 힙 성장이 실제로
   막혔던 그 정확한 시나리오)로 실측 재현 → 수정 후 성공 확인.
2. 3개 이상 리프에 걸친 `store()` 요청은 계속 `false`를 반환하는지
   회귀 테스트로 고정(의도된 스코프 제한이 실수로 풀리지 않게).
3. §2.3의 erase() 경계 검사 실태를 코드로 확인 후, 필요하면 이
   설계에 erase() 절을 보강.
4. 기존 v2 표준 테스트(`PN-38D17292`가 만든 것) 전부 무회귀.

## 5. 참고
- `SP-2AAD7C8D` §3 - MapleTree 원 설계.
- `PN-38D17292`(completed) - v2, 이 한계를 처음 문서화.
- `PN-2EA94B1A` - 이 설계를 요청한 백로그 계획, 실측 트리거 경위 전체.
- `RM-F2DAFF66` §1-X/§1-Y - `Brk()` 첫 실측 및 이 발견의 경위.
- `minicore/libs/libkenv/maple_tree.h/.cpp` - 대상 파일.
