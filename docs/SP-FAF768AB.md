# Minicore 제네릭 컨테이너 템플릿(Node/List/Vector/Rbtree/Map/OrderedList) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-FAF768AB
  status: approved
  updatedAt: 2026-09-17T08:31:49.844Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# Minicore 제네릭 컨테이너 템플릿(Node/List/Vector/Rbtree/Map/OrderedList) — 설계 제안

설계자 메시지(2026-09-17): "List, OrderedList, Node, Rbtree, Map,
Vector에 대해서도 제네릭한 템플릿 클래스를 설계하고, 최적화된
변형들도 추가 설계해."

**이건 완전히 새로운 개념이 아니다** - `ChunkedList<T, ChunkCapacity>`
(libkenv, SP-04EE2A18)가 이미 청크 기반 비침습(non-intrusive) 범용
컨테이너 패턴을 확립해 뒀고, `SharedPtr`/`WeakPtr`/`UniquePtr`
(SP-201238BB)가 침습(intrusive) vs 비침습 두 갈래 설계와
"AllocFn/FreeFn 콜백 주입"/"명시적 init(), placement new 없음" 같은
이 프로젝트의 템플릿 라이브러리 관례를 이미 확립해 뒀다. 이 문서는
그 관례를 그대로 물려받아 **자료구조 컨테이너** 쪽 공백(지금까지
`DelayedExecutionQueue`/대기 큐/`Process::children` 등이 각자 손으로
짠 연결 리스트를 반복해 온 것)을 범용 템플릿으로 일반화한다.

**페이싱 원칙**: `SP-245D130B`/`SP-30FCC8AE` 검토 때 설계자가 남긴
지침("이 기능 자체가 워낙 거대한 기능이라서, 현 단계에서 설계하는
것은 설계 공백을 감당할 수 없을 거야")을 여기도 그대로 따른다 -
6개 컨테이너 + "최적화된 변형"까지 한 번에 전부 확정하지 않고,
**지금 실제로 확정 가능한 기반 4종(Node/List/Vector/Rbtree)**을
먼저 설계하고, 나머지(Map/OrderedList의 정확한 의미, "최적화된
변형"의 구체적 범위)는 §7에서 명시적으로 질의하거나 미룬다.

## 0. 공통 설계 원칙 (기존 템플릿 라이브러리 관례 승계)

- **freestanding, 예외 없음**: `<vector>`/`<map>` 등 표준 컨테이너를
  그대로 쓸 수 없다(RM-23F4B687 §2 - 이 타겟엔 표준 헤더 자체가
  없는 경우가 실측으로 여러 번 확인됨, SP-201238BB 정정 사례 참고).
  삽입/할당 실패는 예외가 아니라 **실패를 나타내는 값**(`nullptr`,
  `false`, 또는 `errno_t`)으로 반환한다 - 어느 쪽인지는 컨테이너별로
  아래에서 정한다.
- **할당자 주입**(`ChunkedList::ensureAllocator` 관례) - 원소 저장에
  힙이 필요한 컨테이너(`Vector`/`List`의 비침습 노드 할당/`Rbtree`의
  비침습 노드 할당)는 `AllocFn`/`FreeFn` 함수 포인터 쌍을 주입받는다
  - 아래 §0-A(`libkcont` 배치) 참고 - `GenericSlabAllocator`(libkmm)의
    존재를 몰라야 하므로(계층 순서, RM-23F4B687 디렉터리 배치 규칙)
    직접 링크하지 않는다.
  - **논블로킹**: 할당 실패 시 절대 블로킹하지 않고 실패를 바로
    반환한다(`ChunkedList::insert()`와 동일한 정책).
- **명시적 초기화, placement new 없음**: 이 코드베이스 전역 관례대로
  raw 슬랩 메모리 위에 얹을 때는 `init()`류 명시적 메서드로 모든
  필드를 초기화한다(전역 `operator new` 자체가 없음, cxxabi.cpp).
- **침습(intrusive) vs 비침습(non-intrusive) 두 갈래**를 명확히
  구분한다(SP-201238BB §2/§2-B가 `SharedPtr`/`IntrusiveSharedPtr`로
  이미 확립한 것과 동일한 원칙) - 아래 각 컨테이너마다 기본은 어느
  쪽인지 명시한다.
- **네이밍**: 자유 함수만 `k`+PascalCase, 클래스 멤버 메서드는 `k`
  없이 camelCase(RM-23F4B687 §1). 클래스/템플릿 이름은 PascalCase.

### 0-A. 배치 - `minicore/libs/libkcont`(신규, `libkenv` 아님) - 커널/유저 공용 + 커널 특수화 계층

**[정정, 2026-09-17, 설계자 의견]** "이건 libkenv 보단
`minicore/libs/libkcont`로 분리하여 커널/유저 공용으로 쓸 수 있도록
만들면 더 낫겠다. 이렇게 만들어서 커널쪽에 `커널 최적화 버전`을
별도로 특수화 하면 되겠네." - 이 문서 초안은 `libkenv`(아키텍처
무관 early 런타임, 커널 전용)에 두는 것으로 썼으나, 설계자가
**새 라이브러리 `minicore/libs/libkcont`**(커널/유저 공용, `libjson`/
`libutf8`/`libelf`와 동일한 매크로 게이팅 없이도 애초에 순수 값
타입/포인터만 다뤄 공용 가능 - RM-7C249618 등재 대상, 착수 시)로
분리하라고 정정했다 - 이 문서의 모든 헤더 경로(`intrusive_list.h`/
`vector.h`/`rbtree.h`/`map.h`)가 `libkcont` 기준으로 갱신됐다(§1-§5A
전체 반영 완료).

**"커널 최적화 버전" 특수화 계층 - 정책(Policy) 템플릿 파라미터**
(2026-09-17, 설계자 의견으로 메커니즘 확정): "'커널 최적화 버전'
특수화 계층은 템플릿 파라미터로 `기능 세트` 구현부를 받아서 공통
구현이 참조하라는 의도로 언급한 것. 기본 구현도 기본 파라미터로
유지하면 기본 사용 사례도 유지 가능해" - 별도 헤더/매크로 게이팅이
아니라 **정책 기반 설계(policy-based design)**: 이 문서 §1-§5의
모든 컨테이너가 `Policy` 템플릿 파라미터를 하나 더 받고, 기본값은
지금까지 설계한 그대로(`AllocFn`/`FreeFn` 콜백 주입, 이식성 우선) -
호출부가 아무것도 안 바꾸면 기존 동작 그대로다. 커널 쪽은 이
파라미터에 `GenericSlabAllocator`/`AtomicPtr<T>`/`Spinlock`을
컴파일 타임에 직접 고정하는 `KernelPolicy`를 넘겨 함수 포인터 간접
호출 1단계를 없앨 수 있다 - **실제 정본 구현은 §3 `Vector<T,
Policy>`**(2026-09-17, 이후 설계자 의견으로 `moveElement`/
`destroyElement`까지 정책 훅으로 확장 - 비trivial 타입 지원도
Policy 책임)가 첫 적용 사례로, `DefaultContainerPolicy`/
`NonTrivialVectorPolicy` 예시도 거기 있다(이 절에서 중복 정의하지
않는다).

**적용 범위**: 이 패턴을 §1-§6의 모든 컨테이너(`List`/`Rbtree`/
`Map`/`ConcurrentMap`/`LockFreeList` 등)에 구체적으로 어떻게
적용할지(어떤 컨테이너가 "정적 함수 vs 런타임 콜백"을 Policy로
추상화할 만큼 가치가 있는지, `Spinlock`류 락 타입까지 Policy로
뽑아낼지)는 착수 세션이 `Vector<T, Policy>`를 첫 사례로 구현하며
구체화한다 - 메커니즘(정책 템플릿 파라미터 + 기본값)은 이제
확정됐으므로 §8 후속 계획으로 분리하지 않고 `PN-633BF2D8`(기반
구현) 자체에 포함한다.

### 0-B. `Traits` 템플릿 파라미터 - Key/KeyOf/Link 다중 파라미터 묶음 (2026-09-17, 설계자 의견)

**[정정, 2026-09-17, 설계자 의견]** "'typename Key, Key (*KeyOf)(const
T&), Node T::*Link' --> 이런 것도 정책(Policy) 템플릿을 받으면 한결
더 단순화 할 수 있을거야." - §1-§6 전반에 걸쳐 반복돼 온 2~4개
템플릿 파라미터(`Key`/`KeyOf`/`Link`)를 컨테이너마다 따로 나열하는
대신, **하나의 `Traits` 구조체**로 묶어 컨테이너 템플릿 시그니처를
전부 `<T, Traits>` 둘로 줄인다 - §0-A의 `Policy`(할당자/이동/소멸
책임)와는 다른 축("이 T가 컨테이너에 어떻게 연결되는가"를 묶음)이지만
같은 패턴(호출부가 작은 구조체 하나로 필요한 정적 정보를 제공)이다.

**두 가지 형태**(컨테이너가 정렬 키를 쓰는지 여부로 갈림):

```cpp
// 연결만 필요한 컨테이너(List/Queue/LruList) - 링크 멤버 포인터
// 하나만 있으면 된다.
template <typename T>
struct MyListTraits {
    static constexpr Node T::* Link = &T::listLink;
};

// 정렬 키까지 필요한 컨테이너(Rbtree/RbMultiTree/OrderedList/
// LockFreeList) - Key 타입 + 키 추출 정적 함수 + 링크 멤버 포인터.
template <typename T>
struct MyTreeTraits {
    using Key = Uid;
    static Key keyOf(const T& item) { return item.uid; }
    static constexpr RbNode T::* Link = &T::treeLink;
};
```

컨테이너 내부에서 이전에 `item->*Link`였던 자리는 `item->*Traits::
Link`로, `KeyOf(*item)`이었던 자리는 `Traits::keyOf(*item)`로
바뀐다 - 순수 시그니처 단순화이고 알고리즘/동작은 바뀌지 않는다.
아래 §1-§6의 모든 컨테이너 선언이 이 형태로 갱신됐다.

## 1. `Node` - 침습적 연결을 위한 최소 링크 구조

```cpp
// minicore/libs/libkcont/intrusive_list.h (신규)
namespace kernel {

// Linux 커널의 struct list_head와 동일한 역할 - T 자신의 메모리
// 안에 이 구조체를 멤버로 내장시켜, 별도 할당 없이(zero-alloc)
// O(1) 삽입/제거가 가능한 이중 연결을 만든다. 원형(circular)
// 이중 연결 리스트 - 비어 있으면 prev==next==自身을 가리키는 sentinel
// 관례(Linux와 동일 - "리스트가 비었는지" 판정과 "맨 끝에 삽입"이
// 둘 다 널 체크 없이 통일된 코드로 처리됨).
struct Node {
    Node* prev = this;
    Node* next = this;

    bool linked() const { return next != this; }

    // 이 노드를 `newNode` 바로 뒤에 끼워 넣는다 - List<T>가 이
    // 프리미티브 위에서 pushFront/pushBack/insertAfter를 구현한다.
    void linkAfter(Node* newNode) {
        newNode->next = next;
        newNode->prev = this;
        next->prev = newNode;
        next = newNode;
    }
    // 이 노드를 리스트에서 뗀다 - 양쪽 이웃을 서로 이어 붙이고,
    // 자기 자신은 다시 sentinel(비어있음) 상태로 되돌린다(이중
    // unlink 방어 - unlink()를 두 번 불러도 안전).
    void unlink() {
        prev->next = next;
        next->prev = prev;
        prev = this;
        next = this;
    }
};

}  // namespace kernel
```

**왜 `T`를 템플릿 인자로 안 받는가**: 표준 `boost::intrusive::list_hook`
류와 달리, 링크 자체는 완전히 타입 무관이다(포인터 산술만 함) -
`T`와의 연결(`Node` 포인터 → `T*`)은 `List<T, Traits>`(§0-B, §2)
쪽이 `Traits::Link` 멤버 포인터로부터 오프셋을 계산한다(Linux의
`container_of` 매크로와 동일한 발상). 이렇게 하면 `T`가 이미 다른 기반 클래스(예:
`IntrusiveRefCounted<T>`)를 상속해도 `Node` 멤버 하나만 더 얹으면
되고, 한 `T`가 서로 다른 리스트 두 개에 동시에 속해야 하는 경우
(예: 전역 프로세스 목록 + 그룹별 목록)도 `Node` 멤버를 두 개 두는
것만으로 자연스럽게 지원된다(다중 상속/vtable 문제 없음).

**[정정, 2026-09-17, PN-73E61BD1 착수 세션 실측 발견] `Node`(또는
`RbNode`/`LockFreeNode` 등 이 문서의 다른 침습적 링크)를 값(value)
단위로 통째로 대입/스왑/복사하는 컨테이너의 원소로는 쓰면 안 된다** -
`T`가 고정 배열의 슬롯처럼 `T tmp = arr[i]; arr[i] = arr[j]; ...`류
memberwise 대입을 겪으면, `Node`의 self-reference 불변조건(빈
상태에서 `prev`/`next`가 자기 자신을 가리킴, §1)이 그 대입 순간
깨진다 - 복사된 포인터가 여전히 "예전 주소"(스왑 전 슬롯 또는 사라진
임시 객체)를 가리키게 되기 때문이다. 실제로 `InterruptSubscriber`
(고정 배열 슬롯, 우선순위 정렬 시 `kSortSubscribers()`가 슬롯 전체를
3-way swap)에 `Queue<AsyncTask, Traits>`를 적용하려다 이 함정을
발견해 착수를 보류했다(`PN-73E61BD1` 항목2 정정 기록 참고) - **포인터/
인덱스로 참조되고 그 자리(슬롯) 자체는 절대 값으로 복사되지 않는
대상**(슬랩에서 개별 할당된 노드, 배열이어도 원소가 다른 곳에서
포인터로만 다뤄지는 경우)에만 침습적 `Node`류를 쓴다 - 값 의미론이
필요한 슬롯에는 기존처럼 포인터/인덱스 기반(`AtomicPtr` 등) 구현을
유지한다.

## 2. `List<T>` - 침습적 이중 연결 리스트 (기본, zero-alloc)

```cpp
// intrusive_list.h (이어서)
namespace kernel {

// `T`가 `Node`를 멤버로 하나 내장하고, 그 멤버 포인터를 `Traits::
// Link`(C++17 non-type template parameter로 멤버 포인터 지원 -
// `&T::link`처럼)로 넘긴다(§0-B) - `offsetof` 매크로 대신 이 방식을
// 쓰는 이유는 표준 매크로 사용(`offsetof`는 non-standard-layout
// 타입에 정의되지 않은 동작)을 피하고, 잘못된 멤버를 넘기면 컴파일
// 타임에 타입 자체가 안 맞아 바로 에러가 나게 하기 위함.
template <typename T, typename Traits>
class List {
public:
    // 비침습 컨테이너들과 통일된 관례상 init()을 두지만, sentinel이
    // 생성자에서 이미 자기 자신을 가리키므로(위 Node 기본값) 사실상
    // 아무 일도 안 한다 - 슬랩 재사용 메모리 위에 얹을 때 명시적으로
    // 다시 부르는 용도로만 존재(ChunkedList와 동일한 방어적 관례).
    void init() { _sentinel = Node{}; }

    void pushBack(T* item) { _sentinel.prev->linkAfter(&(item->*Traits::Link)); }
    void pushFront(T* item) { _sentinel.linkAfter(&(item->*Traits::Link)); }

    // 이 리스트 소속 여부를 List 자신은 추적하지 않는다(Node::unlink()가
    // 이미 이중 호출-안전이므로, "내 리스트가 맞는지" 확인은 호출자
    // 책임 - 표준 intrusive list 라이브러리들과 동일한 절충, 링크당
    // 별도 "소속 리스트" 포인터를 두면 매 삽입/제거마다 여분의 쓰기가
    // 생긴다).
    static void remove(T* item) { (item->*Traits::Link).unlink(); }

    bool empty() const { return !_sentinel.linked(); }

    T* front() const { return empty() ? nullptr : kContainerOf(_sentinel.next); }
    T* back() const { return empty() ? nullptr : kContainerOf(_sentinel.prev); }

    // 전방 반복자 - range-for(`for (T* item : list)`) 지원. 반복
    // 도중 현재 원소를 remove()하는 것은 안전(다음 원소를 미리 캡처)
    // 하지만, 그 외의 리스트 변경은 이 세대의 다른 컨테이너들과
    // 동일하게 정의되지 않은 동작으로 남긴다(이 커널은 반복 중
    // 임의 변경까지 지원하는 세대 카운터를 아직 어디에도 안 씀).
    class Iterator {
    public:
        explicit Iterator(Node* cur) : _cur(cur) {}
        T* operator*() const { return kContainerOf(_cur); }
        Iterator& operator++() { _cur = _cur->next; return *this; }
        bool operator!=(const Iterator& other) const { return _cur != other._cur; }
    private:
        Node* _cur;
    };
    Iterator begin() { return Iterator(_sentinel.next); }
    Iterator end() { return Iterator(&_sentinel); }

private:
    static T* kContainerOf(Node* node) {
        // Node* -> T* : 멤버 포인터 Traits::Link의 실제 바이트 오프셋을
        // 빼는 방식(offsetof 대체, non-standard-layout 안전) - 멤버
        // 포인터를 nullptr 베이스에 적용해 오프셋을 얻는 표준 관용구.
        const uint64_t offset = reinterpret_cast<uint64_t>(&(reinterpret_cast<T*>(0)->*Traits::Link));
        return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(node) - offset);
    }
    Node _sentinel;
};

}  // namespace kernel
```

**기존 손짜기 연결 리스트와의 관계**: `DelayedExecutionQueue`(신규,
SP-F15B4A63)/대기 큐(`Waitable` 구현체들, SP-0666DB3C)가 각자
비슷한 연결 리스트를 손으로 짜 왔다 - 이 문서는 그 코드들을 지금
당장 이 템플릿으로 교체하도록 강제하지 않는다(동작 중인 코드를
검증 없이 건드리지 않는다, RM-23F4B687 §4 - SP-201238BB §3이
`AsyncTaskWeakRef`에 적용한 것과 동일한 원칙). 새로 필요한 연결
리스트부터 이 템플릿을 쓰고, 기존 것은 실제 리팩터 필요성이
생기면(중복 버그 발견, 성능 문제 등) 그때 개별적으로 교체한다.

## 3. `Vector<T>` - 비침습 동적 배열 (기본, 힙 필요)

**[정정, 2026-09-17, 설계자 의견]** "Vector<T>의 비trivial 타입
지원도 역시 정책(Policy) 템플릿 파라미터로 그 책임을 정책을
구현하는 쪽에 넘겨줘." - §0-A가 이미 확정한 정책 템플릿 파라미터
메커니즘을 `Vector<T>` 자신에 실제로 적용한다(§0-A의 예시는 이제
이 절이 정본) - 이전 판본은 "트리비얼 복사 가능 타입만 전제, 비
trivial은 v1 범위 밖"이었으나, 정책이 재할당 시 원소 이동/제거
시 정리를 어떻게 할지를 결정하도록 위임해 **기본 정책은 트리비얼
동작을 그대로 유지**(기존 호출부 무변경)하면서 **비trivial 정책은
호출부가 직접 제공**할 수 있게 한다.

```cpp
// minicore/libs/libkcont/vector.h (신규)
namespace kernel {

// 기본 정책 - 트리비얼 복사 가능한 값 타입 전제(이전 판본과 동일한
// 동작) - moveElement는 단순 대입, destroyElement는 아무것도 안 함
// (소멸자 호출 불필요).
struct DefaultContainerPolicy {
    using AllocFn = void* (*)(uint64_t size);
    using FreeFn = void (*)(void* ptr, uint64_t size);

    template <typename T>
    static void moveElement(T* dst, T* src) { *dst = *src; }
    template <typename T>
    static void destroyElement(T* /*ptr*/) {}
};

template <typename T, typename Policy = DefaultContainerPolicy>
class Vector {
public:
    using AllocFn = typename Policy::AllocFn;
    using FreeFn = typename Policy::FreeFn;

    void ensureAllocator(AllocFn allocFn, FreeFn freeFn) {
        if (_alloc == nullptr) {
            _alloc = allocFn;
            _free = freeFn;
        }
    }

    // 실패(할당 고갈) 시 false, 성공 시 true - 예외 없음(이 프로젝트
    // 관례). ChunkedList와 달리 원소가 연속 메모리에 있어야 하므로
    // 용량 초과 시 전체를 재할당+이동한다(표준 std::vector와 동일한
    // 상환 O(1) 전략 - 2배 증가). 새 원소 자체의 삽입은 복사 대입
    // (`operator=`)으로 - 호출자가 원본 `value`를 계속 들고 있어야
    // 하므로 이동이 아니라 항상 복사(비trivial `T`라도 복사 대입
    // 연산자 자체는 정상 호출됨 - Policy가 관여하는 건 "재할당 중
    // 내부 이동"과 "제거 시 정리"뿐).
    bool pushBack(const T& value) {
        if (_size == _capacity && !grow()) {
            return false;
        }
        _data[_size] = value;
        ++_size;
        return true;
    }

    void popBack() {
        if (_size > 0) {
            --_size;
            Policy::destroyElement(&_data[_size]);  // 기본 정책은 아무 일도
                                                       // 안 함 - 비trivial
                                                       // 정책이 소멸자 호출.
        }
    }

    T& operator[](uint64_t index) { return _data[index]; }
    const T& operator[](uint64_t index) const { return _data[index]; }
    uint64_t size() const { return _size; }
    bool empty() const { return _size == 0; }

    T* begin() { return _data; }
    T* end() { return _data + _size; }

private:
    bool grow() {
        const uint64_t newCapacity = _capacity == 0 ? 4 : _capacity * 2;
        T* newData = static_cast<T*>(_alloc(newCapacity * sizeof(T)));
        if (!newData) {
            return false;
        }
        for (uint64_t i = 0; i < _size; ++i) {
            Policy::moveElement(&newData[i], &_data[i]);  // 기본 정책 =
                // 단순 대입(트리비얼), 비trivial 정책 = 이동 생성자
                // 호출 + 원본 소멸자 호출까지 책임(아래 예시 참고).
        }
        if (_data) {
            _free(_data, _capacity * sizeof(T));
        }
        _data = newData;
        _capacity = newCapacity;
        return true;
    }

    AllocFn _alloc = nullptr;
    FreeFn _free = nullptr;
    T* _data = nullptr;
    uint64_t _size = 0;
    uint64_t _capacity = 0;
};

}  // namespace kernel
```

**비trivial 정책 예시**: 이동 생성자/소멸자가 실제로 있는 타입을
쓰고 싶은 호출부는 `DefaultContainerPolicy` 대신 아래와 같은 정책을
직접 만들어 `Vector<T, NonTrivialVectorPolicy>`처럼 넘긴다 - 기반
`Vector<T>` 코드는 단 한 줄도 안 바뀐다(Policy 뒤로 완전히 위임).

```cpp
// 호출부가 직접 정의(libkcont 밖 - 이동 생성자/소멸자를 실제로
// 호출하는 건 T가 무엇인지 아는 소비자 쪽 책임).
struct NonTrivialVectorPolicy {
    using AllocFn = void* (*)(uint64_t size);
    using FreeFn = void (*)(void* ptr, uint64_t size);

    template <typename T>
    static void moveElement(T* dst, T* src) {
        new (dst) T(kMove(*src));  // 이동 생성(이 프로젝트의 kMove/
                                    // kForward는 type_traits.h 참고,
                                    // SP-201238BB §2 kForward와 동일 계열)
        src->~T();                 // 원본 정리 - moveElement가 "이동 +
                                    // 원본 소멸"까지 한 번에 책임진다
                                    // (grow()는 그 뒤 원본 버퍼 전체를
                                    // 한꺼번에 free만 하므로, 개별 소멸자
                                    // 호출은 반드시 이 안에서 끝나야 함).
    }
    template <typename T>
    static void destroyElement(T* ptr) { ptr->~T(); }
};
```

**주의(정직하게 기록)**: `_data`는 여전히 `_alloc()`이 준 raw 메모리
위에 바로 놓인다(placement new로 처음 구성하지 않음 - `pushBack`의
`_data[_size] = value`는 대상이 이미 생성된 `T` 인스턴스라는 전제로
동작하는 복사 **대입**이지 복사 **생성**이 아니다). 즉 `T`가
비trivial하다면 **`pushBack`이 쓰는 슬롯 자체는 호출부가 미리
`T`의 생성자를 거쳐 채워 둔 상태여야 한다**(이 프로젝트의 기존
"raw 슬랩 메모리 + 명시적 초기화" 관례와 근본적으로 다른 요구라 -
`Vector<T>`에 비trivial `T`를 실제로 쓰려면 이 지점(초기 슬롯 구성)
도 함께 설계해야 한다는 뜻, 착수 세션이 첫 비trivial 소비자가 나올
때 구체화). `moveElement`/`destroyElement` 두 정책 훅은 "이미 살아
있는 원소를 재배치/정리"하는 지점만 다루고, "새 슬롯을 최초로 살아
있게 만드는" 지점은 여전히 이 문서 범위 밖이다.

**`ChunkedList`와의 역할 분담(중요, 혼동 방지)**: 이 둘은 경쟁
관계가 아니라 서로 다른 트레이드오프를 다룬다:
- `Vector<T>` - **연속 메모리**가 필요할 때(포인터 산술로 순회,
  캐시 지역성, C API에 배열로 넘겨야 할 때) - 단, 재할당 시 기존
  포인터/참조가 전부 무효화된다(표준 `std::vector`와 동일한 함정).
- `ChunkedList<T, ChunkCapacity>` - 원소를 가리키는 **안정적인
  포인터**(`Slot*`)가 필요할 때(재할당돼도 기존 `Slot*`가 안
  깨짐) - `Process::children`/`UserThread::PendingSyscall`처럼
  "이 항목을 가리키는 핸들을 오래 들고 있어야 하는" 경우가 기존
  실사용처였다.
둘 다 유지하고, 새 소비자는 이 기준으로 고른다.

## 4. `Rbtree<T>` - 침습적 레드-블랙 트리 (정렬된 O(log n) 조회)

```cpp
// minicore/libs/libkcont/rbtree.h (신규)
namespace kernel {

enum class RbColor : uint8_t { Red, Black };

// List의 Node와 같은 역할이지만 트리용 - 역시 완전히 타입 무관,
// T와의 연결은 Rbtree<T, Traits>(§0-B)가 Traits::Link로 계산.
struct RbNode {
    RbNode* parent = nullptr;
    RbNode* left = nullptr;
    RbNode* right = nullptr;
    RbColor color = RbColor::Red;
};

// `Traits::Key`: 정렬 기준 타입(예: Uid, uint64_t 주소 등, 값 타입
// 비교만 가능하면 됨 - operator< 요구). `Traits::keyOf()`: T에서
// Key를 뽑는 정적 함수, `Traits::Link`: RbNode 멤버 포인터(§0-B) -
// Maple Tree(SP-2AAD7C8D)가 VMA 전용으로 이미 검증한 "커널판 균형
// 이진 트리" 패턴을 범용화한 것이지만, Maple Tree 자체(포인터 태깅 +
// range 노드 4종 - Linux mm 이식)를 대체하지 않는다(§6 범위 밖 -
// VMA는 계속 Maple Tree).
template <typename T, typename Traits>
class Rbtree {
public:
    using Key = typename Traits::Key;

    // find/insert/remove의 실제 회전(rotateLeft/rotateRight)/재색칠
    // 로직은 표준 CLRS 레드-블랙 트리 알고리즘 그대로(자체 발명 없음,
    // RM-23F4B687 §4 - 검증된 알고리즘 재구현, 새 알고리즘 설계
    // 아님) - 착수 세션이 구현.
    T* find(const Key& key) const;      // O(log n), 없으면 nullptr
    bool insert(T* item);               // O(log n), 이미 같은 키가 있으면 false(중복 거부)
    static void remove(T* item);        // O(log n)
    T* first() const;                   // 최솟값(정렬 순회 시작점)
    T* next(T* item) const;             // in-order 후속자 - 정렬 순회용
    bool empty() const { return _root == nullptr; }

private:
    RbNode* _root = nullptr;
};

}  // namespace kernel
```

**왜 Rbtree가 필요한가(실사용처 후보)**: 지금 이 커널에서 "정렬된
키로 O(log n) 조회"가 필요한 자리는 대부분 아직 선형 스캔이거나
(`DelayedExecutionQueue::pump()`가 매번 전체를 훑음, §개선 여지로
이미 문서에 남아있음) 전용 구조(Maple Tree는 VMA 전용)로 막혀
있다 - 향후 `authmgr`의 uid→UserRecord 캐시(정수 키), `RM-48E1E610`
syscall 그룹별 엔트리 테이블(이미 정적 배열로 충분하지만 그룹 수가
늘면 후보), procfs pid 열람(`PN-85FA4992`) 등이 실사용처 후보다 -
**이 문서는 그 소비자들을 강제로 교체하지 않는다**(§2와 동일 원칙).

### 4-A. `RbMultiTree<T>` - 중복 키 허용 변형 (2026-09-17, 설계자 의견으로 분리 명명)

**[정정, 2026-09-17, 설계자 의견]** "Rbtree의 멀티맵(중복 키 허용)은
별도의 다른 이름으로 만들어야 하는 부분이야." - §7이 이전에 "실제
필요해지면 별도 변형"으로 미뤄 뒀던 항목을 지금 명명·설계한다 -
`Rbtree<T>`(§4)에 옵션을 추가하는 대신 **별도 타입**으로 분리한다
(호출부가 타입 이름만 보고 중복 키 허용 여부를 알 수 있게 - `Map`이
"중복 키 없음"을 전제로 쓰이는 것과 섞이지 않도록).

```cpp
// minicore/libs/libkcont/rbtree.h (이어서)
namespace kernel {

// Rbtree<T>(§4)와 노드 구조(RbNode)는 동일하게 재사용하되, insert()가
// 중복 키를 거부하지 않고 항상 성공한다(같은 키의 새 항목은 기존
// 동일 키 그룹의 오른쪽 서브트리에 배치 - 삽입 순서가 in-order
// 순회 순서와 일치하도록, 표준 멀티맵 관용구). `Traits`는 Rbtree(§4)
// 와 동일한 형태(§0-B) - 같은 Traits 타입을 Rbtree/RbMultiTree
// 양쪽에 재사용할 수 있다.
template <typename T, typename Traits>
class RbMultiTree {
public:
    using Key = typename Traits::Key;

    void insert(T* item);  // 항상 성공 - Rbtree::insert와 달리 반환값 없음(실패 케이스가 없어짐)

    // 단일 find() 대신 "같은 키를 가진 전체 구간"을 순회하는 API -
    // 표준 멀티맵의 equal_range와 동일한 발상.
    T* lowerBound(const Key& key) const;  // key 이상인 첫 원소
    T* upperBound(const Key& key) const;  // key 초과인 첫 원소
    // [lowerBound(key), upperBound(key))가 그 키를 가진 전체 구간 -
    // 호출부가 next()(§4 Rbtree와 동일한 in-order 후속자)로 순회.
    T* next(T* item) const;

    static void remove(T* item);  // 특정 인스턴스만 제거(같은 키의 나머지는 유지)
    bool empty() const { return _root == nullptr; }

private:
    RbNode* _root = nullptr;
};

}  // namespace kernel
```

`Map<K,V>`(§5-A)는 여전히 중복 키 없는 `Rbtree` 기반을 유지한다 -
이 `RbMultiTree`는 별도 소비자가 필요할 때(예: 같은 키에 여러 값이
달리는 인덱스류) 쓰는 완전히 독립적인 타입이다.

설계자 답변: "1. Map의 내부 구현은 Rbtree를 이용한 구현이어도 돼.
2. OrderedList는 항상 정렬 상태를 유지하는 리스트가 맞아. 3. List는
삽입 순서 유지하는 리스트. 4. 최근 접근 순서로 재배치하는 리스트는
LruList로 별도 구현하고 5. Lock-free가 가능한 변형들도 설계해야해."

- `List<T>`(§2) - 삽입 순서 유지, 이미 그렇게 설계됨(변경 없음).
- `Map<K,V>` - `Rbtree` 기반 허용됨, §5-A에서 구체화.
- `OrderedList<T>` - 항상 정렬 상태 유지, §5-B에서 구체화.
- `LruList<T>` - 신규 타입(최근 접근 순서 재배치), §5-C에서 구체화.
- Lock-free 변형 - §6에서 난이도별로 다룬다.

### 5-A. `Map<K, V>` - `Rbtree` 기반 정렬 맵

```cpp
// minicore/libs/libkcont/map.h (신규)
namespace kernel {

template <typename K, typename V>
struct MapEntry {
    K key;
    V value;
    RbNode link;
};

// §0-B의 Traits 형태 - Rbtree<T, Traits>가 요구하는 Key/keyOf/Link를
// 한 구조체로 묶는다.
template <typename K, typename V>
struct MapEntryTraits {
    using Key = K;
    static K keyOf(const MapEntry<K, V>& entry) { return entry.key; }
    static constexpr RbNode MapEntry<K, V>::* Link = &MapEntry<K, V>::link;
};

template <typename K, typename V>
using Map = Rbtree<MapEntry<K, V>, MapEntryTraits<K, V>>;

}  // namespace kernel
```

`MapEntry<K,V>`는 호출부가 직접 할당해(스택/슬랩 등, `Rbtree`가
침습적이므로) `insert()`에 넘기는 관례 - "무엇을 침습 대상으로
삼는가"는 `Rbtree` 입장에서 자유이므로, `Map`은 K/V/`RbNode`를 한
구조체에 묶은 전용 `MapEntry`를 그 대상으로 삼아 "Map을 쓰는 코드는
K/V만 다루면 된다"는 편의를 얻는다. `find(key)`가 `Rbtree::find`가
반환하는 `MapEntry*`에서 `.value`만 꺼내는 얇은 wrapper를 추가로
둘지는 착수 시 첫 소비자 기준으로 결정(과설계 방지, RM-23F4B687 §4).

### 5-B. `OrderedList<T>` - 항상 정렬 상태 유지

```cpp
// intrusive_list.h (이어서) 또는 별도 ordered_list.h
namespace kernel {

// List<T,Link>의 Node 링크를 재사용하되, insert()가 항상 정렬 위치를
// 선형 탐색으로 찾아 끼운다 - O(n) 삽입, 순회/first()는 List와 동일
// O(1)/O(n). Rbtree보다 단순하지만 삽입 비용이 원소 수에 비례 -
// 원소 수가 적거나(수십 개 이하) 삽입보다 순회가 훨씬 잦은 경우가
// 적합 대상(Rbtree는 반대로 삽입/조회 모두 잦고 원소 수가 클 때 유리
// - 어느 쪽을 쓸지는 각 소비자가 실측/예상 규모로 판단).
template <typename T, typename Traits>
class OrderedList {
public:
    using Key = typename Traits::Key;

    void init() { _list.init(); }

    // 정렬 위치를 선형 탐색으로 찾아 끼운다(오름차순, operator< 요구).
    void insert(T* item) {
        for (T* cur : _list) {
            if (Traits::keyOf(*item) < Traits::keyOf(*cur)) {
                (cur->*Traits::Link).prev->linkAfter(&(item->*Traits::Link));  // cur 바로 앞에 삽입
                return;
            }
        }
        _list.pushBack(item);  // 끝까지 안 걸리면 맨 뒤(최댓값)
    }
    static void remove(T* item) { List<T, Traits>::remove(item); }
    T* first() const { return _list.front(); }  // 최솟값
    bool empty() const { return _list.empty(); }
    auto begin() { return _list.begin(); }
    auto end() { return _list.end(); }

private:
    List<T, Traits> _list;
};

}  // namespace kernel
```

### 5-C. `LruList<T>` - 최근 접근 순서 재배치 (신규 타입, List/OrderedList와 별개)

```cpp
// intrusive_list.h (이어서) 또는 별도 lru_list.h
namespace kernel {

// List<T,Link> 위에 "접근 시 맨 앞으로 재배치"라는 규약 하나만
// 얹는다 - SP-30FCC8AE §1-A.1의 gUserRecordCache[1024] LRU 축출
// (Hit 시간 기록 + 가장 오래된 것 대체)이 원래 소비자 후보. 이
// 컨테이너는 "무엇이 오래됐는지"만 O(1)로 답할 뿐, 실제 값을 키로
// 찾는 조회(uid -> 항목)는 별도 인덱스(예: §5-A Map)가 필요하면
// 호출부가 따로 관리한다 - 이 컨테이너는 순서만 책임진다.
template <typename T, typename Traits>
class LruList {
public:
    void init() { _list.init(); }

    // 새 항목을 "가장 최근"으로 등록 - 맨 앞에 삽입.
    void insert(T* item) { _list.pushFront(item); }

    // 이미 리스트에 있는 항목을 "방금 접근함"으로 표시 - 맨 앞으로
    // 이동(unlink 후 재삽입, 둘 다 O(1)).
    void touch(T* item) {
        (item->*Traits::Link).unlink();
        _list.pushFront(item);
    }

    // 가장 오래된(맨 뒤) 항목 - 캐시가 꽉 찼을 때 이걸 축출 대상으로
    // 고른다(SP-30FCC8AE의 "Hit 시간이 가장 오래된 것을 대체" 요구와
    // 정확히 대응).
    T* oldest() const { return _list.back(); }
    static void remove(T* item) { List<T, Traits>::remove(item); }
    bool empty() const { return _list.empty(); }

private:
    List<T, Traits> _list;
};

}  // namespace kernel
```

### 5-D. `Queue<T>` - FIFO (2026-09-17, 설계자 의견 "하는 김에 Queue와 LockFreeQueue도 준비해봐")

```cpp
// intrusive_list.h (이어서) 또는 별도 queue.h
namespace kernel {

// List<T,Link> 위에 "뒤에 넣고 앞에서 뺀다"는 FIFO 규약만 얹은 얇은
// 래퍼 - Map(§5-A)이 Rbtree 위에 얹은 것과 동일한 패턴.
template <typename T, typename Traits>
class Queue {
public:
    void init() { _list.init(); }
    void enqueue(T* item) { _list.pushBack(item); }
    T* dequeue() {
        T* item = _list.front();
        if (item) {
            List<T, Traits>::remove(item);
        }
        return item;
    }
    T* front() const { return _list.front(); }
    bool empty() const { return _list.empty(); }

private:
    List<T, Traits> _list;
};

}  // namespace kernel
```

**기존 손짜기 FIFO와의 관계**: `SP-0666DB3C`(Mutex/Semaphore 대기열)
가 이미 FIFO 대기 큐를 손으로 짜 뒀다 - §2 `List`와 동일한 원칙으로,
이 문서는 기존 코드 교체를 강제하지 않는다(RM-23F4B687 §4).

## 6. Lock-free/Concurrent 변형 - 별도 문서로 분리됨

**[분리, 2026-09-17]** 원래 이 절(구 §6~§6.4, Lock-free/Concurrent
변형 - `LockFreeList`/`LockFreeVector`/`ConcurrentMap`/
`ConcurrentRbtree`/`LockFreeQueue`)을 별도 문서 `SP-4DCD0E6A`
("Minicore Lock-free/Concurrent 컨테이너 템플릿")로 독립시켰다 -
이 §1-§5/§0의 기본 컨테이너(전부 `PN-633BF2D8`로 구현 완료)와
달리, Lock-free/Concurrent 변형은 완전히 다른 문제 영역(동시성
프로그래밍)이고 착수 조건(`SP-B1E258D8`/`PN-495C11B7` RCU 완료)도
실행 계획(`PN-013215F9`/`PN-DAE91888`/`PN-A8EF29F7`)도 처음부터
기본 컨테이너와 별도였다 - 한 문서 안에서 "이미 끝난 것"과 "아직
RCU를 기다리는 것"이 섞이면 상태를 혼동하기 쉬워 분리했다. 설계
내용 자체는 전혀 바뀌지 않았다(문구 그대로 이전) - 상세는
`SP-4DCD0E6A` 참고.

## 6-A. [정정, 2026-09-17, PN-633BF2D8 착수 세션 TEMP 검증 중 실측 발견] §2/§4 원안 코드의 실제 버그 2건

착수 세션이 위 §2/§4 코드 스니펫을 그대로 구현한 뒤 kmain.cpp TEMP
단위 테스트 + QEMU(PVH/GRUB SMP4)로 검증하는 과정에서, 이 문서
원안의 예시 코드 자체에 있던 실제 버그 2건을 발견해 구현 코드에서
수정했다(설계 의도는 그대로, 구현 세부만 정정 - RM-23F4B687 §4
원칙과 동일하게 알고리즘/전략을 새로 정하는 게 아니라 명시된 의도를
정확히 구현하기 위한 수정):

1. **`List<T, Traits>::init()`**(§2) - 원안 `void init() { _sentinel
   = Node{}; }`는 `Node{}`로 만들어지는 임시 객체의 NSDMI(`prev/next
   = this`)가 그 임시 객체 자신을 가리키는 self-pointer를 만든다 -
   대입 후 `_sentinel`은 이미 소멸된 임시 객체의 주소(댕글링
   포인터)를 들게 된다(QEMU 실측 - PVH 부팅 직후 `init()` 호출
   직후 `empty()`가 오판, 곧이어 Invalid Opcode 크래시). 필드를
   `_sentinel.prev = &_sentinel; _sentinel.next = &_sentinel;`처럼
   개별 대입해야 진짜 self-referencing sentinel이 된다.
2. **`Rbtree`/`RbMultiTree::remove(T* item)`**(§4/§4-A) - 원안
   시그니처 `static void remove(T* item);`는 `List::remove`와 같은
   static 패턴을 그대로 따랐으나, 레드-블랙 트리는 List와 달리
   완전히 지역적이지 않다 - z 삭제로 실제 루트가 바뀌면(z 자신이
   루트였거나 deleteFixup의 회전이 최상단까지 전파된 경우) 그 새
   루트를 컨테이너 인스턴스의 `_root` 멤버에 반영해야 하는데,
   static 메서드가 z에서 parent를 타고 올라가 찾은 "root"는 그
   함수 호출 동안만 사는 지역 변수라 갱신이 인스턴스에 전파되지
   않는다(QEMU 실측 - 루트 근처 원소 remove() 후에도 find()가
   계속 찾아버리는 트리 손상). 인스턴스 메서드 `void remove(T*
   item)`로 바꿔 `detail::RbCore::remove(&_root, z)`처럼 `_root`를
   직접 갱신하도록 수정.

실제 구현(`minicore/libs/libkcont/intrusive_list.h`/`rbtree.h`,
commit 6c7441e)에는 이미 반영돼 있다 - 이 절은 이 문서의 코드
스니펫과 실제 구현의 차이를 기록해 둔다(CLAUDE.md 규칙 11).

## 7. 범위 밖 (v1)

- **[해소, 2026-09-17, 설계자 의견]** `Vector<T>`의 비trivial 타입
  지원 - 더 이상 범위 밖이 아니다, §3의 `moveElement`/
  `destroyElement` Policy 훅으로 해결됨(기본 `DefaultContainerPolicy`
  는 이전과 동일한 트리비얼 동작 유지, 비trivial이 필요한 호출부가
  `NonTrivialVectorPolicy`류를 직접 제공). 다만 §3이 정직하게 밝힌
  대로, "새 슬롯을 최초로 살아있게 만드는" 지점(placement 생성)은
  여전히 이 문서 범위 밖 - 첫 비trivial 소비자가 나올 때 구체화.
- **`LockFreeOrderedList`/`LockFreeLruList`** - `SP-4DCD0E6A`(분리된
  Lock-free/Concurrent 컨테이너 문서) §5 범위 밖으로 이전됨(`Rbtree`/
  `Map`은 그 문서 §3 정정으로 `ConcurrentRbtree`/`ConcurrentMap`으로
  대체 설계됨 - 더 이상 이 항목이 아님).
- **[해소, 2026-09-17, 설계자 의견]** "SIMD/캐시 라인 정렬 등에서
  언급한 '최적화된 변형'은 앞선 의견 중에, 그 답이 충분하다고
  생각해." - §6(Lock-free/Concurrent 변형)와 §0-A(Policy 템플릿
  파라미터)로 "최적화된 변형" 요구가 이미 충분히 다뤄졌음을 설계자가
  확인 - SIMD/캐시 라인 정렬 등 별도 축은 더 이상 열린 질문이
  아니다(불필요하다는 뜻이 아니라, 이 문서가 이미 제공한 답으로
  충분하다는 확인).

## 8. 후속 계획

- `Node`/`List<T, Traits>`(§1/§2, §0-B), `Vector<T>`(§3), `Rbtree<T,
  Traits>`(§4), `RbMultiTree<T>`(§4-A), `Map<K,V>`(§5-A),
  `OrderedList<T>`(§5-B), `LruList<T>`(§5-C), `Queue<T>`(§5-D) 구현 -
  `PN-633BF2D8`(등록 완료, `RbMultiTree` 포함하도록 갱신 필요).
- **Lock-free/Concurrent 변형(`LockFreeList`/`LockFreeVector`/
  `LockFreeQueue`/`ConcurrentMap`/`ConcurrentRbtree`) 전부 별도 문서로
  분리됨** - `SP-4DCD0E6A` §6(관련 문서/후속 계획) 참고
  (`PN-013215F9`/`PN-DAE91888`/`PN-A8EF29F7`).
- `RM-32D06563`(용어 및 개념)에 `Node`/`List`/`Vector`/`Rbtree`/
  `RbMultiTree`/`Map`/`OrderedList`/`LruList`/`Queue` 등록(Lock-free/
  Concurrent 변형 용어는 `SP-4DCD0E6A`가 자신의 후속 계획으로 등록).
- `RM-7C249618`(라이브러리 목록)에 신규 헤더 등록(`intrusive_list.h`/
  `vector.h`/`rbtree.h`/`map.h`, 전부 `minicore/libs/libkcont`(신설,
  §0 참고), 커널/유저 공용) - 착수 완료 시.
- "커널 최적화 버전" 정책(Policy) 템플릿 파라미터 메커니즘(§0-A) -
  `Vector<T, Policy>`를 첫 적용 사례로 `PN-633BF2D8`에 포함(별도
  후속 문서 아님, 메커니즘 확정 완료).
- `Traits` 템플릿 파라미터 메커니즘(§0-B, Key/keyOf/Link 묶음) -
  이 문서 §1-§6 전 컨테이너 선언에 이미 반영 완료(`List<T, Traits>`/
  `Rbtree<T, Traits>`/`RbMultiTree<T, Traits>`/`OrderedList<T,
  Traits>`/`LruList<T, Traits>`/`Queue<T, Traits>`/`LockFreeList<T,
  Traits>`/`ConcurrentRbtree<T, Traits>`/`LockFreeQueue<T, Traits>`) -
  `PN-633BF2D8`가 착수 시 그대로 구현.
