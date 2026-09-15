#include "wait_queue.h"

#include "scheduler.h"

namespace kernel {

void WaitQueue::parkCurrentAndUnlock(Spinlock& guard) {
    Task* self = Scheduler::currentTask();
    const uint32_t coreIndex = Scheduler::currentCoreIndex();

    _lock.lock();
    self->next.store(nullptr);
    if (_tail) {
        _tail->next.store(self);
    } else {
        _head = self;
    }
    _tail = self;
    self->blockedOn = this;
    self->parkedCoreIndex = coreIndex;
    _lock.unlock();

    // 큐잉이 끝난 뒤에야 상위 프리미티브의 락을 푼다(클래스 주석 참고
    // - 잃어버린 웨이크업 방지).
    guard.unlock();

    Scheduler::parkCurrent();
}

void WaitQueue::wakeOne() {
    _lock.lock();
    Task* task = _head;
    if (task) {
        _head = task->next.load();
        if (!_head) {
            _tail = nullptr;
        }
        task->next.store(nullptr);
        task->blockedOn = nullptr;
    }
    _lock.unlock();

    if (task) {
        Scheduler::scheduleImmediate(task->parkedCoreIndex, task);
    }
}

bool WaitQueue::isEmpty() const {
    return _head == nullptr;
}

bool WaitQueue::cancel(Task* task, WaitCancelReason /*reason*/) {
    _lock.lock();
    Task* prev = nullptr;
    Task* cur = _head;
    while (cur && cur != task) {
        prev = cur;
        cur = cur->next.load();
    }
    if (!cur) {
        _lock.unlock();
        return false;  // 이미 정상적으로 깨어나 떠난 뒤(경쟁 상황)
    }

    Task* next = cur->next.load();
    if (prev) {
        prev->next.store(next);
    } else {
        _head = next;
    }
    if (cur == _tail) {
        _tail = prev;
    }
    cur->next.store(nullptr);
    cur->blockedOn = nullptr;
    _lock.unlock();

    Scheduler::scheduleImmediate(cur->parkedCoreIndex, cur);
    return true;
}

}  // namespace kernel
