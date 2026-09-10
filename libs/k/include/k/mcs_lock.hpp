// mcs_lock — 각 대기자가 자기 소유의 메모리(qnode)만 스핀하는 락
// (ADR-076). 다수/전체 코어가 공유하는 구조에서 경합이 심할 때 쓴다
// (예: 전역 물리 메모리 풀의 노드 간 폴백 경로). 호출자가 스택에
// qnode를 들고 있어야 한다. 전역 스코프(ADR-066).
#pragma once

#include <cstddef>

#include "atomic.hpp"
#include "spinlock.hpp"  // libk_detail::cpu_relax

class mcs_lock {
public:
    struct qnode {
        atomic<qnode*> next{nullptr};
        atomic<bool> locked{false};
    };

    void lock(qnode& my_node) {
        my_node.next.store_relaxed(nullptr);
        qnode* prev = tail_.exchange_acq_rel(&my_node);
        if (prev != nullptr) {
            my_node.locked.store_relaxed(true);
            prev->next.store_release(&my_node);
            while (my_node.locked.load_acquire()) {
                libk_detail::cpu_relax();
            }
        }
    }

    void unlock(qnode& my_node) {
        qnode* next = my_node.next.load_acquire();
        if (next == nullptr) {
            qnode* expected = &my_node;
            if (tail_.compare_exchange_strong_acq_rel(expected, nullptr)) {
                return;
            }
            // 다른 스레드가 이미 enqueue를 시작했지만 아직 next 포인터를
            // 못 채운 사이 구간 — 채워질 때까지 기다린다.
            while ((next = my_node.next.load_acquire()) == nullptr) {
                libk_detail::cpu_relax();
            }
        }
        next->locked.store_release(false);
    }

private:
    atomic<qnode*> tail_{nullptr};
};
