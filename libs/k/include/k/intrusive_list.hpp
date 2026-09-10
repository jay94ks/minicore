// intrusive_list<T, Hook> — 노드가 자신의 링크를 직접 갖는 침습적
// 연결 리스트 (ADR-071). Linux list_head 스타일의 센티널 기반 순환
// 리스트 + 멤버 포인터로 지정하는 훅. objects.md의 프록시 트리,
// memory.md의 free list, scheduler.md의 run_queue가 모두 이것을
// 쓴다(cxx-conventions.md §4). 전역 스코프(ADR-066).
//
// 스레드 안전성 없음 — 락은 이를 감싸는 상위 구조가 소유한다(ADR-033).
#pragma once

#include <cstddef>
#include <cstdint>

struct list_hook {
    list_hook* prev = this;
    list_hook* next = this;
};

template <typename T, list_hook T::*Hook>
class intrusive_list {
public:
    intrusive_list() = default;
    intrusive_list(const intrusive_list&) = delete;
    intrusive_list& operator=(const intrusive_list&) = delete;

    void push_back(T& node) { link_before(&sentinel_, hook_of(node)); }
    void push_front(T& node) { link_before(sentinel_.next, hook_of(node)); }

    // O(1). node가 실제로 이 리스트에 속해 있는지는 호출자가 보장해야 한다.
    static void erase(T& node) {
        list_hook* h = hook_of(node);
        h->prev->next = h->next;
        h->next->prev = h->prev;
        h->prev = h;
        h->next = h;
    }

    bool empty() const { return sentinel_.next == &sentinel_; }

    T& front() { return *node_of(sentinel_.next); }
    T& back() { return *node_of(sentinel_.prev); }

    class iterator {
    public:
        explicit iterator(list_hook* cur) : cur_(cur) {}
        T& operator*() const { return *node_of(cur_); }
        T* operator->() const { return node_of(cur_); }
        iterator& operator++() {
            cur_ = cur_->next;
            return *this;
        }
        bool operator==(const iterator& other) const { return cur_ == other.cur_; }
        bool operator!=(const iterator& other) const { return cur_ != other.cur_; }

    private:
        list_hook* cur_;
    };

    iterator begin() { return iterator(sentinel_.next); }
    iterator end() { return iterator(&sentinel_); }

private:
    static list_hook* hook_of(T& node) { return &(node.*Hook); }

    static T* node_of(list_hook* h) {
        // Hook 멤버 포인터로부터 T 안에서의 바이트 오프셋을 역산해
        // list_hook*를 T*로 되돌린다. 표준이 엄밀히 보장하는 연산은
        // 아니지만 Clang/GCC 모두 지원하는 널리 쓰이는 관용구다
        // (ADR-071, ADR-020이 이 두 컴파일러만 지원 대상으로 정함).
        T* probe = reinterpret_cast<T*>(0x1000);
        auto offset = reinterpret_cast<uintptr_t>(&(probe->*Hook)) - reinterpret_cast<uintptr_t>(probe);
        return reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(h) - offset);
    }

    static void link_before(list_hook* pos, list_hook* h) {
        h->prev = pos->prev;
        h->next = pos;
        pos->prev->next = h;
        pos->prev = h;
    }

    list_hook sentinel_;
};
