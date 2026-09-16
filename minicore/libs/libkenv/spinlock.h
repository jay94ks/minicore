#ifndef MINICORE_LIBS_LIBKENV_SPINLOCK_H
#define MINICORE_LIBS_LIBKENV_SPINLOCK_H

#include "libkenv/types.h"

// libkenv: SMP AP 기동(PL-65C20380) 0단계로 확정된 최소 동시성
// 프리미티브(설계자 지시, QU-B97FDA44, 2026-09-14) - 코어가 2개
// 이상 실제로 돌기 전에 공유 자료구조(Serial, PageFrameAllocator의
// 노드별 buddy 리스트 등)를 보호할 수 있어야 한다. 표준 라이브러리
// <atomic>에 기대지 않고 컴파일러 내장 원자 빌트인만 쓴다 - 헤더
// 하나로 프리스탠딩 어디서든 그대로 쓸 수 있게.

namespace kernel {

// TAS(test-and-set) 기반 스핀락 - 재진입 불가(같은 코어가 이미 든
// 락을 다시 잡으려 하면 그대로 멈춘다, 데드락 방지는 호출부 책임).
class Spinlock {
public:
    void lock() {
        while (__atomic_test_and_set(&_locked, __ATOMIC_ACQUIRE)) {
            while (_locked) {
                asm volatile("pause");  // 버스 트래픽 줄이는 힌트(x86) - 다른 아키텍처에선 무해한 nop 취급
            }
        }
    }

    void unlock() {
        __atomic_clear(&_locked, __ATOMIC_RELEASE);
    }

private:
    uint8_t _locked = 0;
};

// lock()/unlock()을 스코프에 맞춰 자동으로 걸고 푸는 RAII 래퍼.
class SpinlockGuard {
public:
    explicit SpinlockGuard(Spinlock& lock) : _lock(lock) { _lock.lock(); }
    ~SpinlockGuard() { _lock.unlock(); }

    SpinlockGuard(const SpinlockGuard&) = delete;
    SpinlockGuard& operator=(const SpinlockGuard&) = delete;

private:
    Spinlock& _lock;
};

// SMP 기동 동기화(예: "몇 개 AP가 떴는지")에 쓸 최소 원자 카운터.
class AtomicU32 {
public:
    uint32_t load() const {
        return __atomic_load_n(&_value, __ATOMIC_ACQUIRE);
    }

    void store(uint32_t value) {
        __atomic_store_n(&_value, value, __ATOMIC_RELEASE);
    }

    uint32_t fetchAdd(uint32_t delta) {
        return __atomic_fetch_add(&_value, delta, __ATOMIC_ACQ_REL);
    }

    // kernel::string(libkenv/string.h)의 참조 카운트 감소에 쓴다 -
    // 반환값은 감소 전 값(호출부가 "이번에 0으로 떨어졌는지"를
    // fetchSub(1)==1로 판정할 수 있게).
    uint32_t fetchSub(uint32_t delta) {
        return __atomic_fetch_sub(&_value, delta, __ATOMIC_ACQ_REL);
    }

    // lock-free 자료구조(스케줄러 코어별 큐 등, PL-2D3184BC)의 기반 -
    // *expected와 현재 값이 같으면 desired로 바꾸고 true, 다르면
    // *expected를 현재 값으로 갱신하고 false(표준 CAS 관례, weak라
    // 스퓨리어스 실패 가능 - 루프 안에서 재시도하는 용도로만 쓸 것).
    bool compareExchange(uint32_t& expected, uint32_t desired) {
        return __atomic_compare_exchange_n(&_value, &expected, desired, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    // 비트마스크 자료구조(PN-D132A1E9의 수신자별 Target Pending Mask
    // 등)가 락 없이 비트를 세우고/지우는 데 쓴다 - 반환값은 연산 전
    // 값(호출부가 "이번 호출로 실제로 바뀐 비트"를 알고 싶을 때 유용).
    uint32_t fetchOr(uint32_t bits) {
        return __atomic_fetch_or(&_value, bits, __ATOMIC_ACQ_REL);
    }

    uint32_t fetchAnd(uint32_t bits) {
        return __atomic_fetch_and(&_value, bits, __ATOMIC_ACQ_REL);
    }

private:
    uint32_t _value = 0;
};

// lock-free 자료구조가 포인터를 원자적으로 교체할 때 쓴다(예: 큐의
// head/tail, 스택 top) - AtomicU32와 같은 이유로 컴파일러 내장
// 원자 빌트인만 쓴다. 초기값은 nullptr.
template <typename T>
class AtomicPtr {
public:
    T* load() const {
        return __atomic_load_n(&_value, __ATOMIC_ACQUIRE);
    }

    void store(T* value) {
        __atomic_store_n(&_value, value, __ATOMIC_RELEASE);
    }

    T* exchange(T* value) {
        return __atomic_exchange_n(&_value, value, __ATOMIC_ACQ_REL);
    }

    // weak CAS(스퓨리어스 실패 가능) - lock-free 알고리즘 관례대로
    // 루프 안에서 재시도하는 용도. 실패하면 expected가 현재 값으로
    // 갱신된다.
    bool compareExchange(T*& expected, T* desired) {
        return __atomic_compare_exchange_n(&_value, &expected, desired, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

private:
    T* _value = nullptr;
};

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_SPINLOCK_H
