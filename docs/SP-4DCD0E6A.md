# Minicore Lock-free/Concurrent 컨테이너 템플릿(LockFreeList/LockFreeVector/LockFreeQueue/ConcurrentMap/ConcurrentRbtree) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-4DCD0E6A
  status: approved
  updatedAt: 2026-09-17T07:48:57.249Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# Minicore Lock-free/Concurrent 컨테이너 템플릿(LockFreeList/LockFreeVector/LockFreeQueue/ConcurrentMap/ConcurrentRbtree) — 설계 제안

**[분리, 2026-09-17]** `SP-FAF768AB`(Minicore 제네릭 컨테이너 템플릿)
§6(Lock-free 변형)을 별도 문서로 독립시켰다 - 원래 §6은 `SP-FAF768AB`
가 확정한 기본 컨테이너(`Node`/`List`/`Vector`/`Rbtree`/`Map`/
`OrderedList`/`LruList`/`Queue`, 전부 `PN-633BF2D8`로 이미 구현
완료)와 완전히 다른 문제 영역(동시성 프로그래밍 - CAS 루프, 마킹/
헬핑, 세분화 락, hazard 회수)을 다루고, 착수 조건(`SP-B1E258D8`
RCU 완료)도 기본 컨테이너와 무관하게 별도이며, 실행 계획도 처음부터
별도로 분리돼 있었다(`PN-013215F9`/`PN-DAE91888`/`PN-A8EF29F7` vs
`PN-633BF2D8`) - 문서 하나에 "이미 끝난 기반 구현"과 "아직 시작도
못 한 동시성 설계"가 함께 있으면 각자의 상태(완료 vs RCU 대기)를
혼동하기 쉬워 분리한다. 아래 내용은 `SP-FAF768AB` §6/§6.1-§6.4/§7의
해당 항목을 문구 그대로 옮긴 것이며(설계 자체는 바뀌지 않음), 절
번호만 이 문서 기준으로 다시 매겼다.

**선행 지식**: 이 문서의 모든 컨테이너는 `SP-FAF768AB`가 확정한
기반 위에 얹힌다 - `Node`/`List<T,Traits>`(§1/§2), `Rbtree<T,
Traits>`(§4), `Traits`/`Policy` 템플릿 파라미터 메커니즘(§0-A/§0-B),
`Queue<T,Traits>`(§5-D), `Map<K,V>`(§5-A)를 먼저 읽어야 아래 설계가
자연스럽다.

설계자가 "Lock-free가 가능한 변형들도 설계해야해"라고 명시적으로
요구했다(`SP-FAF768AB` §0 페이싱 원칙 - 기반부터 확정 - 은 유지하되,
이 요구를 통째로 범위 밖으로 미루지는 않는다는 방침 그대로). 다만
컨테이너별로 lock-free화의 난이도가 근본적으로 다르므로(SharedPtr의
원자 참조 카운트(SP-201238BB)처럼 "값 하나"를 원자적으로 건드리는
것과, 여러 포인터를 동시에 재연결해야 하는 자료구조 전체를 원자적
일관성으로 유지하는 것은 완전히 다른 문제), 난이도 순으로 나눠
다룬다.

## 1. `LockFreeList<T>` - 실현 가능, 이 문서에서 설계 확정 (Harris 알고리즘)

침습적 단일 연결 리스트에 마킹 비트를 얹는 표준 기법(Harris 2001의
"lock-free linked list" - `SP-FAF768AB` §4가 이미 세운 "검증된 알고리즘
재구현, 자체 발명 아님" 원칙과 동일). 포인터의 최하위 비트 1개를
"논리적으로 삭제됨" 마크로 재사용(포인터는 8바이트 정렬이라 하위
비트가 항상 0 - 이 프로젝트가 Maple Tree(SP-2AAD7C8D)에서 이미
포인터 태깅을 쓴 선례가 있음)하고, 삭제를 "마크만 세우기(CAS 1회)"
+ "물리적 unlink는 다음 순회가 지나가며 청소"의 2단계로 분리한다.

```cpp
// minicore/libs/libkcont/intrusive_list.h(SP-FAF768AB §2와 같은 파일,
// 이어서) - 단일 연결(다음 포인터만 - 이중 연결인 Node와 달리,
// lock-free 알고리즘 자체가 단일 연결을 전제로 함).
namespace kernel {

struct LockFreeNode {
    AtomicPtr<LockFreeNode> next;  // 최하위 비트 = 논리적 삭제 마크
};

template <typename T, typename Traits>
class LockFreeList {
public:
    using Key = typename Traits::Key;

    // insert/find/remove 전부 CAS 루프 - 마킹/헬핑(다른 스레드가
    // 지나가며 마킹된 노드를 대신 청소) 로직은 착수 세션이 Harris
    // 알고리즘 그대로 구현.
    bool insert(T* item);  // 정렬 위치 CAS 삽입 - 키 순서 유지(Harris 원 논문 기준)
    T* find(const Key& key) const;
    static bool remove(T* item);  // 논리적 마킹만 - 물리적 회수는 다음 순회가 처리

private:
    LockFreeNode _head;  // sentinel
};

}  // namespace kernel
```

**전제 조건 - 착수를 막는 진짜 이유**: 마킹된(논리적으로 삭제된)
노드의 실제 메모리 반납 시점이 불확실하다(다른 스레드가 아직 그
노드를 순회 중일 수 있음) - 이 커널에 아직 hazard pointer/RCU류
안전한 회수 메커니즘이 없었다(`SP-B1E258D8` "RCU 인프라 도입 평가"가
`pending`이었던 시절의 바로 그 문제 - 지금은 `approved` + `PN-495C11B7`
`scheduled`로 해소 경로가 열려 있다). **`LockFreeList`의 실제 노드
메모리 반납은 `SP-B1E258D8`(RCU, `PN-495C11B7`)가 실제로 완료되기
전까지는 안전하게 구현할 수 없다** - 설계(위 스케치)는 확정하되,
착수는 `PN-495C11B7` 완료 이후로 미룬다(§6 착수 조건에 명시).

## 2. `LockFreeVector<T>` - 부분적으로 가능, append-only로 범위 제한

전체 재할당이 필요한 표준 `Vector`(`SP-FAF768AB` §3)의 성장 전략은
lock-free와 근본적으로 안 맞는다(재할당 중 다른 스레드의 읽기가 옛
버퍼를 볼 수도 새 버퍼를 볼 수도 있어 단순 CAS로 못 막음) - 대신
**세그먼트 방식**(세그먼트 크기가 2배씩 커지는 고정 크기 세그먼트들의
배열, 세그먼트 자체는 한 번 할당되면 절대 재할당/이동하지 않음 -
`pushBack`은 새 세그먼트가 필요할 때만 그 포인터를 CAS로 게시)이면
기존 원소를 가리키는 포인터가 절대 무효화되지 않으면서 lock-free
append가 가능하다(잘 알려진 기법 - 여러 언어의 concurrent 컬렉션
구현이 같은 계열의 세그먼트 전략을 씀). **다만 임의 인덱스
`erase`/축소는 lock-free로 지원하지 않는다**(append-only 전용) -
v1 lock-free 변형은 append-only로 스코프를 좁힌다.

## 3. `ConcurrentMap<K,V>`/`ConcurrentRbtree<T>` - ConcurrentDictionary 패턴 (2026-09-17, 설계자 의견으로 정정)

**[정정, 2026-09-17, 설계자 의견]** "Rbtree나 Map의 Lock-free 구현은
C#의 ConcurrentDictionary와 유사한 형태로 구현 계획을 수립해." - 이전
판단("자체 lock-free 균형 트리 알고리즘은 활발한 연구 주제라 이 문서
범위 밖")을 정정한 것. `ConcurrentDictionary`는 사실 **완전한
lock-free가 아니라 "세분화된 락(bucket당 락) + 완전 lock-free 읽기"
하이브리드**다(정확한 내부 동작: 버킷 배열 + 개수 고정된 락 배열
(버킷 인덱스를 락 개수로 나눈 나머지가 담당 락) - 읽기(`TryGetValue`
등)는 락을 전혀 안 잡고 volatile 읽기로 순회, 쓰기(추가/삭제/갱신)만
해당 버킷 담당 락을 잡아 **서로 다른 버킷을 건드리는 쓰기끼리는
완전 병렬**, 리사이즈만 예외적으로 전체 락을 순서대로 잡음) - 이건
§1/§2의 "포인터 하나를 CAS"하는 진짜 lock-free와는 다른, **더
실용적이고 이 코드베이스에서 바로 구현 가능한 패턴**이다.

**정직한 비대칭 인정**: `ConcurrentDictionary`의 버킷들은 서로 완전히
독립적(버킷 3 쓰기가 버킷 7 구조에 영향 없음)이라 버킷별 락이
자연스럽게 병렬화되지만, **레드-블랙 트리는 회전(rotation)이 전역
균형 불변조건에 의존**해 "이 서브트리만 잠그고 저 서브트리는
자유롭게"가 구조적으로 성립하지 않는다 - 그래서 `Map`과 `Rbtree`를
분리해서 다룬다:

- **`ConcurrentMap<K,V>`** - **해시 테이블 기반으로 재설계**(더 이상
  `SP-FAF768AB` §5-A의 `Rbtree` 기반이 아니다 - `ConcurrentDictionary`
  자신도 해시 테이블이지 정렬 트리가 아니고, "정렬 유지"는 원래
  `Map`의 요구사항이 아니었다 - 키로 값을 빠르게 찾는 게 목적).
  버킷 배열 + 고정 개수 `Spinlock` 배열(버킷 인덱스 % 락 개수) +
  버킷 내부는 침습적 단일 연결 체인(충돌 해결, `LockFreeNode`류 -
  §1과 동일 마킹 삭제, in-place 수정 없음이라 락 없는 읽기가 안전).
  리사이즈(버킷 배열 확장)는 전체 락을 정해진 순서로 잡고 재해싱
  (`ConcurrentDictionary`와 동일한 예외 케이스 - 드물고 비싸지만
  평상시 병렬성을 해치지 않음).
- **`ConcurrentRbtree<T>`** - 버킷처럼 독립적으로 세분화할 수 없으므로
  **단일 전역 쓰기 락 + lock-free 읽기**로 절충한다 - 쓰기(insert/
  remove/rotate)는 전부 하나의 락으로 직렬화되지만(`ConcurrentMap`
  만큼 쓰기가 병렬화되지 않는다는 한계를 정직하게 인정), 읽기(find/
  first/next 순회)는 그 락과 무관하게 항상 락 없이 진행한다 - 트리
  변경은 "새 서브트리를 먼저 구성한 뒤 마지막에 원자적 포인터 1회
  교체로 게시"하는 방식이어야(단순 in-place 회전과 다름) 읽기가
  교체 전/후 중 하나의 일관된 트리만 보고 중간 상태를 절대 못 본다
  (RCU의 "writer copies, then atomically republishes" 원칙을 노드
  단위로 적용).

```cpp
// minicore/libs/libkcont/concurrent_map.h (신규)
namespace kernel {

template <typename K, typename V, uint32_t LockStripes = 16>
class ConcurrentMap {
public:
    bool find(const K& key, V* outValue) const;  // 락 없음
    bool insert(const K& key, const V& value);    // kLockFor(key)만 잡음
    bool remove(const K& key);                    // 마찬가지

private:
    Spinlock* kLockFor(uint64_t bucketIndex) { return &_locks[bucketIndex % LockStripes]; }
    // 버킷 배열/리사이즈/해시 함수는 착수 세션이 구현 - 이 커널에
    // 범용 해시 함수가 아직 없다(§5 참고).
    Spinlock _locks[LockStripes];
};

}  // namespace kernel
```

```cpp
// minicore/libs/libkcont/rbtree.h (이어서, SP-FAF768AB §4에 추가)
namespace kernel {

template <typename T, typename Traits>
class ConcurrentRbtree {
public:
    using Key = typename Traits::Key;

    T* find(const Key& key) const;  // 락 없음 - Rbtree::find와 동일 순회
    bool insert(T* item) {
        SpinlockGuard guard(_writeLock);
        return _tree.insert(item);
    }
    static void remove(T* item);    // 쓰기 락 필요

private:
    Rbtree<T, Traits> _tree;
    Spinlock _writeLock;
};

}  // namespace kernel
```

**`ConcurrentRbtree`도 `SP-B1E258D8`(RCU, `PN-495C11B7`) 완료 없이는
착수 불가** - §1과 동일한 전제 조건(옛 서브트리를 읽던 리더가 아직
있는데 그 메모리를 반납하면 use-after-free) - `ConcurrentMap`도 버킷
체인 삭제가 §1과 같은 마킹 방식이라 동일 전제 조건을 공유한다.

**남은 미확정 사항(착수 시 확정)**: 해시 함수 전략(이 커널에 범용
해시 함수가 아직 없음), 리사이즈 임계치/배율, `Rbtree`(`SP-FAF768AB`
§4) 회전 구현이 "새 서브트리 구성 후 원자적 1회 교체"를 실제로
지원하도록 바뀌어야 하는지(단순 in-place 회전과 다른 요구라 §4 기반
구현 자체에 영향을 줄 수 있음 - 착수 세션이 `Rbtree`/`ConcurrentRbtree`
관계를 재검토).

**`OrderedList`/`LruList`는 이 정정의 대상이 아니다**(설계자 의견이
"Rbtree나 Map"만 지목) - `OrderedList`(`SP-FAF768AB` §5-B, 정렬 삽입
탐색이 여러 노드를 순서대로 읽어야 해 버킷 독립성도 Harris류 단일
연결 마킹도 적용 안 됨)와 `LruList`(`SP-FAF768AB` §5-C, `touch()`가
unlink+재삽입 두 단계라 원자적으로 안 묶이면 중간에 다른 스레드가 그
항목을 못 찾는 창이 생김)는 여전히 이 문서에서 lock-free/concurrent
설계를 제공하지 않는다 - 이 판단이 틀렸다면 정정 바란다.

## 4. `LockFreeQueue<T>` - 실현 가능, 이 문서에서 설계 확정 (Michael-Scott 알고리즘)

`Queue`(`SP-FAF768AB` §5-D)의 lock-free 버전은 Michael-Scott 1996
알고리즘(MPMC lock-free FIFO 큐) - `LockFreeList`(§1)의 Harris
알고리즘과 같은 급의 확립된 표준 기법이라 "실현 가능" 등급이다. 핵심
아이디어: 항상 더미(dummy) sentinel 노드 하나를 유지해 큐가 절대
완전히 비지 않게 하고, `enqueue`는 tail을 CAS로 전진, `dequeue`는
head를 CAS로 전진(head가 가리키던 옛 더미 노드가 다음 더미가 됨) -
enqueue/dequeue 두 CAS 지점이 서로 겹쳐도(한쪽이 tail을 아직 못
따라잡은 "느슨한 tail" 상태) 안전하게 헬핑으로 보정되는 것이 이
알고리즘의 핵심.

```cpp
// intrusive_list.h (이어서) 또는 별도 lockfree_queue.h
namespace kernel {

template <typename T, typename Traits>
class LockFreeQueue {
public:
    // head/tail 둘 다 항상 더미 노드를 하나 가리킨다(초기화 시 더미
    // 하나를 확보해 _head=_tail=dummy로 시작 - 착수 세션이 더미 노드
    // 확보 방식(정적 멤버 vs 호출자 제공)을 확정).
    bool enqueue(T* item);       // CAS로 tail 전진, 실패 시 재시도(락 없음)
    T* dequeue();                // CAS로 head 전진, 비어있으면 nullptr

private:
    AtomicPtr<LockFreeNode> _head;
    AtomicPtr<LockFreeNode> _tail;
};

}  // namespace kernel
```

**`LockFreeList`(§1)와 동일한 전제 조건**: dequeue된 옛 head 노드의
실제 메모리 반납이 다른 스레드의 진행 중인 enqueue/dequeue와 겹칠 수
있어(그 스레드가 아직 그 노드를 통해 tail을 따라가는 중일 수 있음),
`SP-B1E258D8`(RCU, `PN-495C11B7`)가 실제로 완료되기 전까지는 안전하게
구현할 수 없다 - 착수 조건은 `LockFreeList`와 동일하게 `PN-495C11B7`
완료(§6).

## 5. 범위 밖 (v1)

- **`LockFreeOrderedList`/`LockFreeLruList`** - §3 근거로 이 문서
  범위 밖(`Rbtree`/`Map`은 §3 정정으로 `ConcurrentRbtree`/
  `ConcurrentMap`으로 대체 설계됨 - 더 이상 이 항목이 아니다).
  `OrderedList`/`LruList`(`SP-FAF768AB` §5-B/§5-C) 자체가 lock-free화
  하기 어려운 이유는 §3 말미 참고.

## 6. 관련 문서 / 후속 계획

- `LockFreeList<T>`(§1)/`LockFreeQueue<T>`(§4) - `PN-013215F9`(등록
  완료, `PN-633BF2D8`(libkcont 기반) 완료 + `PN-495C11B7`(RCU) 완료가
  착수 조건, plan_depend 연결 완료).
- `LockFreeVector<T>`(§2, append-only 세그먼트 방식) - `PN-DAE91888`
  (등록 완료, `PN-633BF2D8` 완료가 착수 조건 - RCU 의존 없음, append-
  only라 회수 문제 자체가 없음).
- `ConcurrentMap<K,V>`/`ConcurrentRbtree<T>`(§3, ConcurrentDictionary
  패턴) - `PN-A8EF29F7`(등록 완료, `PN-633BF2D8` 완료 + `PN-495C11B7`
  (RCU) 완료가 착수 조건, plan_depend 연결 완료).
- `RM-32D06563`(용어 및 개념)에 `LockFreeList`/`LockFreeQueue`/
  `LockFreeVector`/`ConcurrentMap`/`ConcurrentRbtree` 등록 - 각 PN
  착수 완료 시.
- 기반 타입(`Node`/`List`/`Vector`/`Rbtree`/`Traits`/`Policy` 등)은
  `SP-FAF768AB`가 소유 - 이 문서는 그 위에 얹히는 동시성 변형만
  다룬다.
- RCU 자체(`Rcu`/`RcuReadGuard`/`RcuCallback`)는 `SP-B1E258D8`/
  `PN-495C11B7`이 소유 - 이 문서의 모든 착수 조건이 공유하는 그
  선행 조건의 설계/구현은 그 문서들 책임이다.

