// result<T, E> — 성공 값 T 또는 실패 값 E 중 정확히 하나를 보관하는
// 태그드 유니온 (ADR-068). <variant>/<expected>는 freestanding 허용
// 목록 밖이라 수동 저장(alignas 버퍼 + placement new)으로 직접
// 구현한다. 전역 스코프(ADR-066) — libk::result가 아니라 그냥 result.
//
// 알려진 단순화(M3 "최소 구현" 범위): ADR-068은 복사 생성자를 T/E가
// 둘 다 복사 가능할 때만 SFINAE로 활성화하라고 정했지만, 이 구현은
// 무조건 선언한다 — 복사 불가능한 T/E에 대해 실제로 복사를 시도할
// 때만(템플릿이 실제로 그 멤버를 ODR-use할 때만) 컴파일 에러가 난다.
// 아직 result<T,E> 자체의 복사 가능 여부를 타입 특성으로 질의하는
// 코드가 없어 당장은 관찰 가능한 차이가 없다.
#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "panic.hpp"

template <typename T, typename E>
class result {
public:
    static result ok(T value) { return result(std::move(value), ok_tag{}); }
    static result err(E error) { return result(std::move(error), err_tag{}); }

    result(result&& other) noexcept : has_value_(other.has_value_) {
        if (has_value_) {
            ::new (storage_) T(std::move(other.value_ref()));
        } else {
            ::new (storage_) E(std::move(other.error_ref()));
        }
    }

    result(const result& other) : has_value_(other.has_value_) {
        if (has_value_) {
            ::new (storage_) T(other.value_ref());
        } else {
            ::new (storage_) E(other.error_ref());
        }
    }

    result& operator=(result&& other) noexcept {
        if (this != &other) {
            destroy();
            has_value_ = other.has_value_;
            if (has_value_) {
                ::new (storage_) T(std::move(other.value_ref()));
            } else {
                ::new (storage_) E(std::move(other.error_ref()));
            }
        }
        return *this;
    }

    ~result() { destroy(); }

    bool is_ok() const { return has_value_; }
    bool is_err() const { return !has_value_; }
    explicit operator bool() const { return is_ok(); }

    T& value() {
        if (!has_value_) {
            LIBK_PANIC("result::value() called on err");
        }
        return value_ref();
    }
    const T& value() const {
        if (!has_value_) {
            LIBK_PANIC("result::value() called on err");
        }
        return value_ref();
    }

    E& error() {
        if (has_value_) {
            LIBK_PANIC("result::error() called on ok");
        }
        return error_ref();
    }
    const E& error() const {
        if (has_value_) {
            LIBK_PANIC("result::error() called on ok");
        }
        return error_ref();
    }

    T value_or(T fallback) && {
        if (has_value_) {
            return std::move(value_ref());
        }
        return std::move(fallback);
    }

private:
    struct ok_tag {};
    struct err_tag {};
    result(T value, ok_tag) : has_value_(true) { ::new (storage_) T(std::move(value)); }
    result(E error, err_tag) : has_value_(false) { ::new (storage_) E(std::move(error)); }

    T& value_ref() { return *reinterpret_cast<T*>(storage_); }
    const T& value_ref() const { return *reinterpret_cast<const T*>(storage_); }
    E& error_ref() { return *reinterpret_cast<E*>(storage_); }
    const E& error_ref() const { return *reinterpret_cast<const E*>(storage_); }

    void destroy() {
        if constexpr (!std::is_trivially_destructible_v<T> || !std::is_trivially_destructible_v<E>) {
            if (has_value_) {
                value_ref().~T();
            } else {
                error_ref().~E();
            }
        }
    }

    static constexpr size_t k_storage_size = sizeof(T) > sizeof(E) ? sizeof(T) : sizeof(E);
    static constexpr size_t k_storage_align = alignof(T) > alignof(E) ? alignof(T) : alignof(E);
    alignas(k_storage_align) unsigned char storage_[k_storage_size];
    bool has_value_;
};

// T = void 특수화: 값 저장 없이 E + has_value_만 유지한다.
template <typename E>
class result<void, E> {
public:
    static result ok() {
        result r;
        r.has_value_ = true;
        return r;
    }
    static result err(E error) {
        result r;
        r.has_value_ = false;
        ::new (r.storage_) E(std::move(error));
        return r;
    }

    result(result&& other) noexcept : has_value_(other.has_value_) {
        if (!has_value_) {
            ::new (storage_) E(std::move(other.error_ref()));
        }
    }

    result(const result& other) : has_value_(other.has_value_) {
        if (!has_value_) {
            ::new (storage_) E(other.error_ref());
        }
    }

    ~result() { destroy(); }

    bool is_ok() const { return has_value_; }
    bool is_err() const { return !has_value_; }
    explicit operator bool() const { return is_ok(); }

    void value() const {
        if (!has_value_) {
            LIBK_PANIC("result<void,E>::value() called on err");
        }
    }

    E& error() {
        if (has_value_) {
            LIBK_PANIC("result<void,E>::error() called on ok");
        }
        return error_ref();
    }
    const E& error() const {
        if (has_value_) {
            LIBK_PANIC("result<void,E>::error() called on ok");
        }
        return error_ref();
    }

private:
    result() = default;

    E& error_ref() { return *reinterpret_cast<E*>(storage_); }
    const E& error_ref() const { return *reinterpret_cast<const E*>(storage_); }

    void destroy() {
        if constexpr (!std::is_trivially_destructible_v<E>) {
            if (!has_value_) {
                error_ref().~E();
            }
        }
    }

    alignas(E) unsigned char storage_[sizeof(E)];
    bool has_value_ = true;
};
