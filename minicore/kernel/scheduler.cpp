#include "scheduler.h"

#include "acpi.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"

namespace kernel {

void TaskQueue::pushBack(Task* task) {
    SpinlockGuard guard(_lock);
    task->next.store(nullptr);
    if (_tail) {
        _tail->next.store(task);
    } else {
        _head = task;
    }
    _tail = task;
}

void TaskQueue::pushFront(Task* task) {
    SpinlockGuard guard(_lock);
    task->next.store(_head);
    _head = task;
    if (!_tail) {
        _tail = task;
    }
}

Task* TaskQueue::popFront() {
    SpinlockGuard guard(_lock);
    Task* task = _head;
    if (task) {
        _head = task->next.load();
        if (!_head) {
            _tail = nullptr;
        }
        task->next.store(nullptr);
    }
    return task;
}

bool TaskQueue::isEmpty() const {
    return _head == nullptr;
}

namespace {

constexpr uint32_t kMaxCores = kAcpiMaxCpus;
TaskQueue gQueues[kMaxCores];
uint32_t gCoreCount = 1;

}  // namespace

void Scheduler::init() {
    gCoreCount = Acpi::cpuCount();
    if (gCoreCount == 0) {
        gCoreCount = 1;
    }
    if (gCoreCount > kMaxCores) {
        gCoreCount = kMaxCores;
    }
}

void Scheduler::enqueue(uint32_t coreIndex, Task* task) {
    if (coreIndex >= gCoreCount) {
        coreIndex = 0;
    }
    gQueues[coreIndex].pushBack(task);
}

void Scheduler::scheduleImmediate(uint32_t coreIndex, Task* task) {
    if (coreIndex >= gCoreCount) {
        coreIndex = 0;
    }
    gQueues[coreIndex].pushFront(task);
}

Task* Scheduler::pickNext(uint32_t coreIndex) {
    if (coreIndex >= gCoreCount) {
        coreIndex = 0;
    }
    return gQueues[coreIndex].popFront();
}

}  // namespace kernel
