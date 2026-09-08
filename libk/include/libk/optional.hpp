// optional<T> — 값이 없을 수 있지만 "실패"는 아닌 경우를 표현한다
// (ADR-069). result<T,E>(result.hpp)와 같은 수동 저장 기법(alignas
// 버퍼 + placement new)을 쓴다 — 코드 중복은 두 타입뿐이라 지금은
// 감수한다(세 번째 유사 타입이 생기면 공통화를 검토). 전역
// 스코프(ADR-066).
#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "panic.hpp"

template <typename T>
class optional {
public:
    optional() : has_value_(false) {}
    optional(T value) : has_value_(true) { ::new (storage_) T(std::move(value)); }
    static optional none() { return optional(); }

    optional(optional&& other) noexcept : has_value_(other.has_value_) {
        if (has_value_) {
            ::new (storage_) T(std::move(other.value_ref()));
        }
    }

    optional(const optional& other) : has_value_(other.has_value_) {
        if (has_value_) {
            ::new (storage_) T(other.value_ref());
        }
    }

    optional& operator=(optional&& other) noexcept {
        if (this != &other) {
            reset();
            has_value_ = other.has_value_;
            if (has_value_) {
                ::new (storage_) T(std::move(other.value_ref()));
            }
        }
        return *this;
    }

    ~optional() { reset(); }

    bool has_value() const { return has_value_; }
    explicit operator bool() const { return has_value(); }

    T& value() {
        if (!has_value_) {
            LIBK_PANIC("optional::value() called on empty optional");
        }
        return value_ref();
    }
    const T& value() const {
        if (!has_value_) {
            LIBK_PANIC("optional::value() called on empty optional");
        }
        return value_ref();
    }

    T value_or(T fallback) && {
        if (has_value_) {
            return std::move(value_ref());
        }
        return std::move(fallback);
    }

    template <typename... Args>
    T& emplace(Args&&... args) {
        reset();
        ::new (storage_) T(std::forward<Args>(args)...);
        has_value_ = true;
        return value_ref();
    }

    void reset() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            if (has_value_) {
                value_ref().~T();
            }
        }
        has_value_ = false;
    }

private:
    T& value_ref() { return *reinterpret_cast<T*>(storage_); }
    const T& value_ref() const { return *reinterpret_cast<const T*>(storage_); }

    alignas(T) unsigned char storage_[sizeof(T)];
    bool has_value_;
};
