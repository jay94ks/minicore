// futex_wait/futex_wake — pthread 최소 구현의 동기화 기초
// (real-libc-syscall-layer.md §M37, ADR-187 §결정3). `FUTEX_WAIT`/
// `FUTEX_WAKE`만 다룬다(`FUTEX_CMP_REQUEUE` 등 고급 연산은 범위 밖).
// 주소별로 정확히 나누지 않고 호출자의 address_space 전체에 대기열
// 하나만 둔다(kernel_objects.hpp::address_space::futex_waiters 주석
// 참고 — YAGNI, 프로세스당 스레드 수가 원래 적다).
#pragma once

#include <cstdint>

namespace kern::arch::x86_64 {

enum class futex_error : uint32_t {
    ok = 0,
    not_a_user_process,
    value_mismatch,  // *uaddr != expected — 블록하지 않고 즉시 반환.
};

// sys_futex(a1=uaddr, a2=FUTEX_WAIT, a3=expected) — *uaddr가 expected와
// 다르면 즉시 value_mismatch. 같으면 다른 스레드의 futex_wake(같은
// uaddr)가 깨울 때까지 블록한다(kern::sched::block()).
futex_error futex_wait(uint64_t uaddr, uint32_t expected);

// sys_futex(a1=uaddr, a2=FUTEX_WAKE, a3=max_count) — 이 address_space의
// futex_waiters에서 uaddr과 일치하는 대기자를 최대 max_count명
// run_queue로 되돌린다(kern::sched::enqueue). 실제로 깨운 수를
// 반환한다.
uint32_t futex_wake(uint64_t uaddr, uint32_t max_count);

}  // namespace kern::arch::x86_64
