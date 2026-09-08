// ticket_lock — FIFO 공정성을 보장하는 락 (ADR-076). 공정성(기아 방지)이
// 필요한 중간 경합(예: NUMA 노드별 run_queue 락)에 쓴다. 전역
// 스코프(ADR-066).
#pragma once

#include <cstdint>

#include "atomic.hpp"
#include "spinlock.hpp"  // libk_detail::cpu_relax

class ticket_lock {
public:
    void lock() {
        uint32_t my_ticket = next_.fetch_add_relaxed(1);
        while (now_serving_.load_acquire() != my_ticket) {
            libk_detail::cpu_relax();
        }
    }

    void unlock() { now_serving_.store_release(now_serving_.load_relaxed() + 1); }

private:
    atomic<uint32_t> next_{0};
    atomic<uint32_t> now_serving_{0};
};
