// span<T> — 소유권 없는 연속 메모리 뷰(포인터+길이) (ADR-070). IPC로
// 오가는 원시 버퍼를 표현하는 데 쓴다. 경계 검사는 항상 수행한다(끄는
// 매크로 없음 — 성능보다 정확성 우선, ADR-001). 전역 스코프(ADR-066).
#pragma once

#include <cstddef>

#include "panic.hpp"

template <typename T>
class span {
public:
    span() = default;
    span(T* data, size_t count) : data_(data), size_(count) {}

    template <size_t N>
    span(T (&arr)[N]) : data_(arr), size_(N) {}

    // 다른 span<U>로부터의 변환 — U*가 T*로 암시적 변환 가능할 때만
    // 컴파일된다(예: span<uint8_t> → span<const uint8_t>는 되지만
    // 반대 방향은 U*→T* 변환 자체가 언어 규칙상 막힌다). U가 T와
    // 같을 때는 컴파일러가 아래 대신 비템플릿 복사 생성자를 우선
    // 선택한다.
    template <typename U>
    span(const span<U>& other) : data_(other.data()), size_(other.size()) {}

    T& operator[](size_t i) const {
        if (i >= size_) {
            LIBK_PANIC("span::operator[] index out of range");
        }
        return data_[i];
    }

    T* data() const { return data_; }
    size_t size() const { return size_; }
    size_t size_bytes() const { return size_ * sizeof(T); }
    bool empty() const { return size_ == 0; }

    span subspan(size_t offset, size_t count) const {
        if (offset > size_ || count > size_ - offset) {
            LIBK_PANIC("span::subspan out of range");
        }
        return span(data_ + offset, count);
    }

    T* begin() const { return data_; }
    T* end() const { return data_ + size_; }

private:
    T* data_ = nullptr;
    size_t size_ = 0;
};
