#ifndef MINICORE_LIBS_LIBKENV_SPINLOCK_H
#define MINICORE_LIBS_LIBKENV_SPINLOCK_H

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
    unsigned char _locked = 0;
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
    unsigned int load() const {
        return __atomic_load_n(&_value, __ATOMIC_ACQUIRE);
    }

    void store(unsigned int value) {
        __atomic_store_n(&_value, value, __ATOMIC_RELEASE);
    }

    unsigned int fetchAdd(unsigned int delta) {
        return __atomic_fetch_add(&_value, delta, __ATOMIC_ACQ_REL);
    }

private:
    unsigned int _value = 0;
};

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_SPINLOCK_H
