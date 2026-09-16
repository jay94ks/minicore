#ifndef MINICORE_KERNEL_WAIT_QUEUE_H
#define MINICORE_KERNEL_WAIT_QUEUE_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "task.h"
#include "waitable.h"

namespace kernel {

// SP-0666DB3C §1 - Mutex/Semaphore가 공유하는 FIFO 대기열. Task::next
// (스케줄러 큐 전용 침습적 포인터지만, 파킹된 Task는 어느 스케줄러
// 큐에도 없으므로 재사용 가능)로 단일 연결 리스트를 구성한다.
// WaitQueue 자신이 Waitable이므로, 이 큐에 파킹된 Task의 blockedOn은
// (Mutex/Semaphore 자신이 아니라) 이 인스턴스를 직접 가리킨다.
class WaitQueue : public Waitable {
public:
    // 현재 Task를 큐 꼬리에 매달고 파킹한다 - 이 함수는 반환하지 않다가
    // wakeOne()이나 cancel()이 이 Task를 다시 스케줄링해야 반환한다.
    // 호출 전 guard(상위 프리미티브의 Spinlock)를 잠근 채로 불러야
    // 하며, 이 함수가 큐잉을 마친 뒤에야 그 락을 대신 풀고 파킹한다
    // (Mesa 모니터 패턴 - 락을 쥔 채로 파킹하면 다른 코어가 영원히
    // 그 락을 못 잡는다). "큐에 매달기"와 "락 해제"를 원자적으로 묶는
    // 이유: 그 사이에 unlock()/release()가 끼어들면 아직 큐에 없는
    // 우리를 못 보고 지나가 버리는 잃어버린 웨이크업이 된다(채널 IPC
    // 다중 waiter 수정, PN-C9625015에서 겪은 것과 같은 종류의 경쟁).
    void parkCurrentAndUnlock(Spinlock& guard);

    // 큐 머리에서 하나 꺼내 즉시 재개시킨다(Scheduler::scheduleImmediate
    // 재사용 - 파킹된 Task는 이중 스케줄링 걱정이 없다고 이미 문서화돼
    // 있음). 이 Task가 깨어나서 재시도했을 때 실패할 수 있다(다른 새
    // 호출자가 먼저 채갔을 경우, §2.1 재경쟁 철학) - 그러면 다시
    // 파킹된다. 비어 있으면 아무 일도 안 함.
    void wakeOne();

    // 대기 중인 전부를 한 번에 재개시킨다(SP-F682B889 §9.5-4,
    // AsyncTaskCompletion::_syncWaiters의 완료 통지처럼 "누가 먼저
    // 잡느냐" 경쟁이 아니라 브로드캐스트 의미일 때 쓴다 - wakeOne()의
    // "재경쟁" 철학과 달리 큐에 있던 Task 전부가 그대로 깨어난다).
    // 비어 있으면 아무 일도 안 함.
    void wakeAll();

    // 진단용 스냅샷(정확한 값이 필요하면 호출부가 락을 별도로 잡아야
    // 함 - scheduler.h의 TaskQueue::isEmpty와 같은 관례).
    bool isEmpty() const;

    // Waitable 구현(§9.3) - task가 이 큐 안에 있으면 O(n) 순회로
    // 리스트에서 잘라내고 강제로 재개시킨다(리스트가 대개 짧다는 전제
    // - 경합이 심한 락은 애초에 설계 재검토 대상, §5-2와 같은 가정).
    // 이미 정상적으로 깨어나 떠난 뒤라면(경쟁 상황) 아무 일도 하지
    // 않고 false.
    bool cancel(Task* task, WaitCancelReason reason) override;

private:
    Spinlock _lock;  // _head/_tail/각 Task의 blockedOn 정리를 보호(짧게만 보유)
    Task* _head = nullptr;
    Task* _tail = nullptr;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_WAIT_QUEUE_H
