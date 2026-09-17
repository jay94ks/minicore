#ifndef MINICORE_LIBS_LIBKCONT_INTRUSIVE_LIST_H
#define MINICORE_LIBS_LIBKCONT_INTRUSIVE_LIST_H

#include "libkenv/types.h"

// libkcont: 커널/유저 공용 제네릭 컨테이너 템플릿 라이브러리
// (SP-FAF768AB §0-§5, PN-633BF2D8) - 이 헤더는 Node(§1)/List<T,
// Traits>(§2)와, 그 위에 얇게 얹은 OrderedList<T, Traits>(§5-B)/
// LruList<T, Traits>(§5-C)/Queue<T, Traits>(§5-D)를 담는다. 전부
// 침습적(intrusive, T 자신이 링크 멤버를 내장) - 별도 할당 없이
// (zero-alloc) O(1) 삽입/제거를 목표로 한다.
//
// Traits 관례(SP-FAF768AB §0-B): 이 파일의 모든 컨테이너는 `Node
// T::*Link` 멤버 포인터 하나만 필요하다(정렬 키가 필요한 컨테이너는
// rbtree.h 쪽 Traits 형태 참고) - 예:
//
//   template <typename T>
//   struct MyListTraits {
//       static constexpr Node T::* Link = &T::listLink;
//   };
//
// 정렬 키가 필요한 OrderedList는 여기에 Key/keyOf도 추가로 요구한다
// (아래 OrderedList 선언 참고).

namespace kernel {

// Linux 커널의 struct list_head와 동일한 역할 - T 자신의 메모리 안에
// 이 구조체를 멤버로 내장시켜, 별도 할당 없이(zero-alloc) O(1)
// 삽입/제거가 가능한 이중 연결을 만든다. 원형(circular) 이중 연결
// 리스트 - 비어 있으면 prev==next==自身을 가리키는 sentinel 관례
// (Linux와 동일 - "리스트가 비었는지" 판정과 "맨 끝에 삽입"이 둘 다
// 널 체크 없이 통일된 코드로 처리됨).
struct Node {
    Node* prev = this;
    Node* next = this;

    bool linked() const { return next != this; }

    // 이 노드를 `newNode` 바로 뒤에 끼워 넣는다 - List<T, Traits>가
    // 이 프리미티브 위에서 pushFront/pushBack/insertAfter를 구현한다.
    void linkAfter(Node* newNode) {
        newNode->next = next;
        newNode->prev = this;
        next->prev = newNode;
        next = newNode;
    }

    // 이 노드를 리스트에서 뗀다 - 양쪽 이웃을 서로 이어 붙이고, 자기
    // 자신은 다시 sentinel(비어있음) 상태로 되돌린다(이중 unlink
    // 방어 - unlink()를 두 번 불러도 안전).
    void unlink() {
        prev->next = next;
        next->prev = prev;
        prev = this;
        next = this;
    }
};

// `T`가 `Node`를 멤버로 하나 내장하고, 그 멤버 포인터를 `Traits::
// Link`(C++17 non-type template parameter로 멤버 포인터 지원 -
// `&T::link`처럼)로 넘긴다(SP-FAF768AB §0-B) - `offsetof` 매크로
// 대신 이 방식을 쓰는 이유는 표준 매크로 사용(`offsetof`는
// non-standard-layout 타입에 정의되지 않은 동작)을 피하고, 잘못된
// 멤버를 넘기면 컴파일 타임에 타입 자체가 안 맞아 바로 에러가 나게
// 하기 위함.
template <typename T, typename Traits>
class List {
public:
    // 비침습 컨테이너들과 통일된 관례상 init()을 두지만, sentinel이
    // 생성자에서 이미 자기 자신을 가리키므로(위 Node 기본값) 사실상
    // 아무 일도 안 한다 - 슬랩 재사용 메모리 위에 얹을 때 명시적으로
    // 다시 부르는 용도로만 존재(ChunkedList와 동일한 방어적 관례).
    //
    // [수정, 2026-09-17, PN-633BF2D8 TEMP 검증 중 실측 발견] `_sentinel
    // = Node{}`(SP-FAF768AB §2 원안)는 버그다 - `Node{}`는 그 자리의
    // 임시 Node 객체를 하나 만드는데, 그 임시 객체의 NSDMI(`prev =
    // this`)가 가리키는 `this`는 `_sentinel`이 아니라 그 임시 객체
    // 자신의 주소다. 대입 후 `_sentinel.prev`/`next`는 이미 소멸된
    // 임시 객체의 주소(스택 재사용으로 금방 쓰레기가 됨)를 들고 있게
    // 돼, 이후 첫 pushBack()/empty() 호출이 곧바로 댕글링 포인터를
    // 역참조한다(QEMU 실측 - PVH 부팅 직후 Invalid Opcode 즉시 크래시,
    // "List: 초기 empty()" 실패 로그로 먼저 드러남). 자기 자신을
    // 직접 가리키도록 필드를 개별 대입해야 한다.
    void init() {
        _sentinel.prev = &_sentinel;
        _sentinel.next = &_sentinel;
    }

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
    // 동일하게 정의되지 않은 동작으로 남긴다(이 커널은 반복 중 임의
    // 변경까지 지원하는 세대 카운터를 아직 어디에도 안 씀).
    class Iterator {
    public:
        explicit Iterator(Node* cur) : _cur(cur) {}
        T* operator*() const { return kContainerOf(_cur); }
        Iterator& operator++() {
            _cur = _cur->next;
            return *this;
        }
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

// List<T, Traits>(위)의 Node 링크를 재사용하되, insert()가 항상
// 정렬 위치를 선형 탐색으로 찾아 끼운다(SP-FAF768AB §5-B) - O(n)
// 삽입, 순회/first()는 List와 동일 O(1)/O(n). Rbtree(rbtree.h)보다
// 단순하지만 삽입 비용이 원소 수에 비례 - 원소 수가 적거나(수십 개
// 이하) 삽입보다 순회가 훨씬 잦은 경우가 적합 대상.
//
// Traits는 List와 달리 Link 외에 정렬 키도 필요하다(rbtree.h의
// Traits 형태와 동일) - Key 타입 + keyOf() 정적 함수 + Link.
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

// List<T, Traits>(위) 위에 "접근 시 맨 앞으로 재배치"라는 규약 하나만
// 얹는다(SP-FAF768AB §5-C) - 이 컨테이너는 "무엇이 오래됐는지"만
// O(1)로 답할 뿐, 실제 값을 키로 찾는 조회(uid -> 항목)는 별도
// 인덱스(예: map.h의 Map)가 필요하면 호출부가 따로 관리한다 - 이
// 컨테이너는 순서만 책임진다. Traits는 List와 동일(Link만 필요,
// 정렬 키 불필요).
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
    // 고른다.
    T* oldest() const { return _list.back(); }
    static void remove(T* item) { List<T, Traits>::remove(item); }
    bool empty() const { return _list.empty(); }

private:
    List<T, Traits> _list;
};

// List<T, Traits>(위) 위에 "뒤에 넣고 앞에서 뺀다"는 FIFO 규약만
// 얹은 얇은 래퍼(SP-FAF768AB §5-D) - map.h의 Map이 Rbtree 위에
// 얹은 것과 동일한 패턴. Traits는 List와 동일(Link만 필요).
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

#endif  // MINICORE_LIBS_LIBKCONT_INTRUSIVE_LIST_H
