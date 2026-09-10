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

// M34(real-libc-syscall-layer.md §M34, ADR-185) 실행 중 발견한 진짜
// 버그: `state_`는 이 락 객체 하나에 딱 하나뿐인데, 예전엔
// `lock()`이 뮤텍스를 잡기 **전에** 거기 자기 코어의 irq 상태를 써
// 넣었다 — 이 락이 정말로 여러 코어에서 동시에 다투기 전(M21~M33은
// BSP 하나만 이 락들을 만졌다)에는 절대 겹칠 수 없어 드러나지 않던
// 데이터 경합이다. M34가 run_queue::lock을 처음으로 진짜 멀티코어
// 경합에 노출시키자, 두 코어가 거의 동시에 `state_`를 덮어써 서로의
// (또는 자신의) irq 상태를 잘못 복원하는 것을 실제로 겪었다(한 코어가
// IF=1이어야 할 자리에 IF=0을 복원해 그 코어가 다시는 타이머로
// 깨어나지 못하고 멈춤 — SMP 스모크 테스트가 뽑는 fpu 데모 완료
// 로그 이후로 아무 로그도 더 안 나오는 정지로 드러났다).
//
// 고침: 뮤텍스를 **먼저** 잡고(스핀 중에는 인터럽트를 끄지 않는다 —
// 아직 아무것도 소유하지 않은 상태라 이 코어가 그 사이 선점돼도
// 위험하지 않다, 나중에 이 스핀을 다시 이어서 하면 그만이다), 그
// 다음에야 이 코어의 irq 상태를 `state_`에 저장한다 — 이 시점부터는
// 이 락을 배타적으로 소유한 코어만 `state_`를 건드리므로(다른
// 코어는 아직 Lock::lock()에서 스핀 중이라 절대 여기 도달하지
// 못한다) 더 이상 경합이 없다. unlock()도 대칭적으로, 뮤텍스를
// 놓기 **전에** `state_`를 지역변수로 복사해 둔다(놓은 직후부터는
// 다른 코어가 그 필드를 또 덮어쓸 수 있으므로).
template <typename Lock>
class irq_safe : private Lock {
public:
    void lock() {
        Lock::lock();
        state_ = libk_detail::arch_irq_save();
    }
    void unlock() {
        libk_detail::irq_state saved = state_;
        Lock::unlock();
        libk_detail::arch_irq_restore(saved);
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
