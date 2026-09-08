// atomic<T> — 메모리 순서를 함수 이름에 고정한 원자적 접근 래퍼
// (ADR-072). "실수로 seq_cst를 쓰다가 나중에 발견"되는 버그를 막으려고
// std::memory_order를 호출부 인자로 받는 대신 연산+순서 조합을 메서드
// 이름에 박아 넣는다. 필요해지는 조합만 추가한다(전 조합을 미리 만들지
// 않는다).
//
// 이 툴체인에는 freestanding 타깃용 <atomic>이 없다(ADR-113) —
// std::atomic<T> 대신 GCC/Clang 공통 컴파일러 내장(__atomic_*)을 직접
// 쓴다(ADR-115: shim이 비현실적인 헤더는 libk 자체 구현으로 대체).
// 관찰 가능한 API·의미론은 ADR-072와 동일하다.
//
// 생성자를 constexpr로 만드는 이유(cxx-conventions.md §1의 "전역 정적
// 객체의 동적 초기화 금지" 요구사항, ADR-010): 이 크로스 툴체인
// 산출물은 crt0가 없어 .init_array(C++ 전역 생성자 호출 목록)를 아무도
// 실행하지 않는다 — atomic<T>(그리고 이를 멤버로 갖는 spinlock 등)가
// constexpr 생성자를 갖지 못하면, 이를 포함하는 전역 변수(예:
// spinlock을 멤버로 둔 구조체 배열)가 "동적 초기화가 필요하다"고
// 컴파일러가 판단해 실행되지 않는 전역 생성자에 초기화를 떠넘기고,
// 그 필드는 (0이 아니어야 할 값이라도) .bss의 0으로 남는다 — 실제로
// M3에서 이 문제로 슬랩 힙의 청크 크기가 0으로 남아 나눗셈 예외(#DE)가
// 났다. mutable 대신 const_cast를 쓰는 이유도 같다 — mutable 멤버가
// 있으면 상수 초기화 자격을 잃는다.
#pragma once

template <typename T>
class atomic {
public:
    constexpr atomic() = default;
    constexpr explicit atomic(T v) : value_(v) {}

    atomic(const atomic&) = delete;
    atomic& operator=(const atomic&) = delete;

    T load_relaxed() const { return __atomic_load_n(ptr(), __ATOMIC_RELAXED); }
    T load_acquire() const { return __atomic_load_n(ptr(), __ATOMIC_ACQUIRE); }

    void store_relaxed(T v) { __atomic_store_n(ptr(), v, __ATOMIC_RELAXED); }
    void store_release(T v) { __atomic_store_n(ptr(), v, __ATOMIC_RELEASE); }

    T fetch_add_relaxed(T delta) { return __atomic_fetch_add(ptr(), delta, __ATOMIC_RELAXED); }

    T exchange_acq_rel(T v) { return __atomic_exchange_n(ptr(), v, __ATOMIC_ACQ_REL); }

    // expected/desired 비교·교체. 실패 시 expected는 관찰된 현재값으로
    // 갱신된다(표준 CAS 관용).
    bool compare_exchange_strong_acq_rel(T& expected, T desired) {
        return __atomic_compare_exchange_n(ptr(), &expected, desired, /*weak=*/false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

private:
    T* ptr() const { return const_cast<T*>(&value_); }

    T value_{};
};
