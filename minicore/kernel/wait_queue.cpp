#include "wait_queue.h"

#include "scheduler.h"

namespace kernel {

void WaitQueue::parkCurrentAndUnlock(Spinlock& guard, const WeakPtr<Waitable>& selfAsWaitable) {
    Task* self = Scheduler::currentTask();
    const uint32_t coreIndex = Scheduler::currentCoreIndex();

    _lock.lock();
    _queue.enqueue(self);
    self->blockedOn = selfAsWaitable;
    self->parkedCoreIndex = coreIndex;
    // §9.6-3 - 이번 파킹을 새로 시작하는 시점에 리셋한다(지난 번
    // 파킹에서 취소됐던 낡은 값이 이번 파킹에도 남아 있으면 안 됨) -
    // cancel()이 실제로 취소할 때만 이 뒤에 덮어쓴다.
    self->lastCancelReason = WaitCancelReason::None;
    _lock.unlock();

    // 큐잉이 끝난 뒤에야 상위 프리미티브의 락을 푼다(클래스 주석 참고
    // - 잃어버린 웨이크업 방지).
    guard.unlock();

    Scheduler::parkCurrent();
}

void WaitQueue::wakeOne() {
    _lock.lock();
    Task* task = _queue.dequeue();
    if (task) {
        task->blockedOn = WeakPtr<Waitable>();
    }
    _lock.unlock();

    if (task) {
        Scheduler::scheduleImmediate(task->parkedCoreIndex, task);
    }
}

void WaitQueue::wakeAll() {
    // [수정, 2026-09-17, PN-73E61BD1 항목1] 원래 코드는 _head/_tail
    // 포인터 두 개를 스왑하는 것만으로 리스트 전체를 O(1)에 통째로
    // 떼어냈다 - libkcont Queue<T,Traits>는 그런 "전체 스왑" API가
    // 없어(대신 dequeue()로 한 번에 하나씩만 뗀다) 락을 쥔 채로
    // dequeue()를 반복해 전부 비운다(그래도 원소 개수만큼의 O(1)
    // 연산 - 여전히 잠금 구간 안에서 리스트를 완전히 비운 뒤에만
    // 락을 풀고 밖에서 순회한다는 원래의 동시성 계약은 그대로
    // 지킨다). 비운 항목들은 스택 지역 변수인 이 임시 Queue에 옮겨
    // 담아, 락 밖에서 안전하게 순회한다.
    Queue<Task, WaitQueueTraits> drained;
    _lock.lock();
    for (;;) {
        Task* task = _queue.dequeue();
        if (!task) {
            break;
        }
        drained.enqueue(task);
    }
    _lock.unlock();

    // 이 시점부터 이 인스턴스(_queue)는 이미 빈 큐이므로, 아래 순회
    // 중 blockedOn을 아직 못 지운 Task를 노리는 동시 cancel() 호출이
    // 있어도 _queue에서 찾지 못해 안전하게 false를 반환한다(wakeOne()/
    // cancel()과 같은 "경쟁 시 조용히 무시" 계약).
    for (;;) {
        Task* task = drained.dequeue();
        if (!task) {
            break;
        }
        task->blockedOn = WeakPtr<Waitable>();
        Scheduler::scheduleImmediate(task->parkedCoreIndex, task);
    }
}

bool WaitQueue::isEmpty() const {
    return _queue.empty();
}

bool WaitQueue::cancel(Task* task, WaitCancelReason reason) {
    _lock.lock();
    // [수정, 2026-09-17, PN-73E61BD1 항목1] 예전엔 _head부터 O(n)
    // 순회하며 task를 찾아야만 그 앞뒤를 이어붙일 수 있었으나,
    // libkcont의 침습적 Node는 이웃만 알면 되므로(어느 리스트 몇 번째
    // 인지 몰라도) 순회 자체가 필요 없다 - 다만 "이 task가 지금 정말
    // 이 큐(또는 임의의 다른 리스트)에 링크돼 있는지"는 확인해야
    // 한다(이미 정상적으로 깨어나 waitQueueLink가 unlink된 상태라면
    // 이 큐가 아니라 다른 어떤 리스트에도 안 걸려 있다는 뜻).
    if (!task->waitQueueLink.linked()) {
        _lock.unlock();
        return false;  // 이미 정상적으로 깨어나 떠난 뒤(경쟁 상황)
    }
    List<Task, WaitQueueTraits>::remove(task);
    task->blockedOn = WeakPtr<Waitable>();
    // §9.6-3(설계 문서 pseudocode, 지금까지 미구현이었음) - 재개된
    // 코드가 "정상 웨이크업"과 "강제로 끌려나옴"을 구분할 수 있게.
    task->lastCancelReason = reason;
    _lock.unlock();

    Scheduler::scheduleImmediate(task->parkedCoreIndex, task);
    return true;
}

}  // namespace kernel
