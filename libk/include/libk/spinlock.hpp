// spinlock — test-and-test-and-set(TTAS) (ADR-076). 경합이 드문 짧은
// 임계구역의 기본 선택(엔드포인트별 IPC 대기열, 디버그 콘솔 전역 로그
// 등). 전역 스코프(ADR-066).
#pragma once

#include <cstdint>

#include "atomic.hpp"

namespace libk_detail {

// 스핀 중 캐시라인 경합을 줄이는 아키텍처별 힌트. x86_64 `pause`,
// aarch64 `yield` — 컴파일 타임에 타깃으로 선택되므로 arch 헤더
// include 없이도 libk 안에 안전하게 둘 수 있다.
inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    asm volatile("pause");
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

}  // namespace libk_detail

class spinlock {
public:
    void lock() {
        while (true) {
            // test: CAS 없이 일반 로드로만 스핀하다가 unlocked로 보일 때만
            // 실제 CAS를 시도한다 — 캐시라인 경합을 줄인다(TTAS).
            while (state_.load_relaxed() != 0) {
                libk_detail::cpu_relax();
            }
            uint32_t expected = 0;
            if (state_.compare_exchange_strong_acq_rel(expected, 1)) {
                return;
            }
        }
    }

    bool try_lock() {
        uint32_t expected = 0;
        return state_.compare_exchange_strong_acq_rel(expected, 1);
    }

    void unlock() { state_.store_release(0); }

private:
    atomic<uint32_t> state_{0};  // 0 = unlocked, 1 = locked
};
