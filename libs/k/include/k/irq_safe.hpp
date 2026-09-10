// irq_safe<Lock> — 인터럽트 핸들러와도 경합할 수 있는 문맥에서 쓰는
// 락 래퍼(spinlock/ticket_lock/mcs_lock 중 어느 것이든 감쌀 수 있다).
// scoped_lock<Lock>도 여기 함께 둔다 — 특정 락 타입에 종속되지 않는
// 유틸리티라 "타입 1개당 헤더 1개" 원칙(ADR-066)의 예외로 공용
// 헤더에 모은다(ADR-076). 전역 스코프(ADR-066).
#pragma once

#include <cstdint>

namespace libk_detail {

using irq_state = uintptr_t;

// libk는 선언만 한다 — 커널 arch 계층만 정의를 제공한다(ADR-076).
// 서버(유저 프로세스)는 이 심벌을 정의하지 않으므로, 유저 코드가
// 실수로 irq_safe<Lock>을 쓰면 링크 에러로 즉시 드러난다.
irq_state arch_irq_save();
void arch_irq_restore(irq_state state);

}  // namespace libk_detail

template <typename Lock>
class irq_safe : private Lock {
public:
    void lock() {
        state_ = libk_detail::arch_irq_save();
        Lock::lock();
    }
    void unlock() {
        Lock::unlock();
        libk_detail::arch_irq_restore(state_);
    }

private:
    libk_detail::irq_state state_ = 0;
};

template <typename Lock>
class scoped_lock {
public:
    explicit scoped_lock(Lock& l) : lock_(l) { lock_.lock(); }
    ~scoped_lock() { lock_.unlock(); }
    scoped_lock(const scoped_lock&) = delete;
    scoped_lock& operator=(const scoped_lock&) = delete;

private:
    Lock& lock_;
};
