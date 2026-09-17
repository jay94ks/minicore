#ifndef MINICORE_LIBS_LIBKCONT_VECTOR_H
#define MINICORE_LIBS_LIBKCONT_VECTOR_H

#include "libkenv/types.h"

// libkcont: Vector<T, Policy>(SP-FAF768AB §3, §0-A) - 비침습 동적
// 배열. 연속 메모리가 필요할 때(포인터 산술로 순회, 캐시 지역성, C
// API에 배열로 넘겨야 할 때) 쓴다 - 단, 재할당 시 기존 포인터/참조가
// 전부 무효화된다(표준 std::vector와 동일한 함정). 원소를 가리키는
// *안정적인* 포인터가 필요하면 대신 libkenv/chunked_list.h의
// ChunkedList<T, ChunkCapacity>를 쓴다(재할당돼도 기존 Slot*가 안
// 깨짐) - 둘은 경쟁 관계가 아니라 서로 다른 트레이드오프를 다룬다.

namespace kernel {

// 기본 정책 - 트리비얼 복사 가능한 값 타입 전제 - moveElement는 단순
// 대입, destroyElement는 아무것도 안 함(소멸자 호출 불필요). 비trivial
// 타입을 다루고 싶은 호출부는 이 정책 대신 moveElement에서 이동
// 생성+원본 소멸자를, destroyElement에서 소멸자를 직접 호출하는 정책을
// 만들어 Vector<T, MyPolicy>처럼 넘긴다(Vector 자신의 코드는 한 줄도
// 안 바뀜, Policy 뒤로 완전히 위임) - libkcont는 이 프로젝트에 아직
// 없는 이동 생성자/placement new 관용구를 강제하지 않는다.
struct DefaultContainerPolicy {
    using AllocFn = void* (*)(uint64_t size);
    using FreeFn = void (*)(void* ptr, uint64_t size);

    template <typename T>
    static void moveElement(T* dst, T* src) {
        *dst = *src;
    }
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
                // 호출 + 원본 소멸자 호출까지 책임.
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

#endif  // MINICORE_LIBS_LIBKCONT_VECTOR_H
