#ifndef MINICORE_KERNEL_WAIT_QUEUE_H
#define MINICORE_KERNEL_WAIT_QUEUE_H

#include "libkcont/intrusive_list.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "task.h"
#include "waitable.h"

namespace kernel {

// [수정, 2026-09-17, PN-73E61BD1 항목1, SP-FAF768AB §5-D] Task::
// waitQueueLink(task.h)를 링크로 쓰는 Traits - 예전엔 스케줄러 큐
// 전용 `Task::next`를 재사용하는 손짜기 단일 연결 리스트였으나,
// libkcont `Queue<T, Traits>`(§5-D, List<T,Traits> 위의 FIFO 래퍼)로
// 교체했다(순수 내부 자료구조 치환 - 공개 API/의미 불변, RM-23F4B687
// §4 "동작 중인 코드를 검증 없이 건드리지 않는다" 원칙에 따라 항목별
// 개별 검증 완료).
struct WaitQueueTraits {
    static constexpr Node Task::* Link = &Task::waitQueueLink;
};

// SP-0666DB3C §1 - Mutex/Semaphore가 공유하는 FIFO 대기열.
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
    //
    // [수정, 2026-09-17, PN-B41D8C0E] `selfAsWaitable`은 호출자(Mutex/
    // Semaphore)가 자기 자신의 컨트롤 블록을 별칭(aliasing)해 만든,
    // 이 WaitQueue 인스턴스를 가리키는 WeakPtr<Waitable> - 그대로
    // Task::blockedOn에 심어 강제 cancel()(§9.5)이 대상을 찾을 수
    // 있게 한다. 호출자가 kMakeShared로 안 만들어졌으면(컨트롤 블록
    // 없음) 빈 WeakPtr을 넘겨도 안전하다(task.h의 blockedOn 주석 참고
    // - 파킹 자체는 이 필드와 무관하게 정상 동작, 강제 cancel()만
    // 무력화됨).
    void parkCurrentAndUnlock(Spinlock& guard, const WeakPtr<Waitable>& selfAsWaitable);

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

    // Waitable 구현(§9.3) - task가 이 큐 안에 있으면(자기 자신을
    // 가리키지 않는 waitQueueLink) 잘라내고 강제로 재개시킨다.
    // [수정, 2026-09-17, PN-73E61BD1 항목1] 예전엔 O(n) 순회로 직접
    // 찾아 스플라이스했으나, libkcont `List<T,Traits>::remove()`가
    // 대상 노드 자체의 이웃만 다시 잇는 진짜 O(1) 연산이라(어느
    // 리스트의 몇 번째인지 몰라도 됨) 이 함수도 그만큼 빨라졌다(순수
    // 자료구조 치환의 부수 효과 - 새 알고리즘을 설계한 것이 아니라
    // libkcont가 이미 그렇게 구현돼 있음, PN-633BF2D8). 이미 정상적으로
    // 깨어나 떠난 뒤라면(경쟁 상황, waitQueueLink가 이미 unlink된
    // 상태) 아무 일도 하지 않고 false.
    bool cancel(Task* task, WaitCancelReason reason) override;

    // [신규, 2026-09-19, PN-0AC554C2] `WaitQueue` 자신은 여러 Task가
    // 공유하는 큐라 "이 큐 자체가 완료됐는가"라는 인스턴스 단위 상태를
    // 갖지 않는다(각 Task별 완료 여부는 여전히 이 큐/`blockedOn`에서
    // 빠졌는지로만 판단 - wakeOne()/wakeAll()/cancel()이 그 자리에서
    // 직접 처리). 그래서 항상 `false`를 반환하는 자리표시자다 - 진짜
    // per-task 완료 신호가 필요해지면(PN-0AC554C2 2단계, Mutex/
    // Semaphore를 새 구조로 마이그레이션할 때) 이 공유 `WaitQueue`
    // 대신 "이 큐에 파킹된 특정 Task 하나"를 표현하는 전용 per-wait
    // `Waitable` 구현체로 교체될 예정이다.
    bool isCompleted() const override { return false; }

private:
    Spinlock _lock;  // _queue/각 Task의 blockedOn 정리를 보호(짧게만 보유)
    Queue<Task, WaitQueueTraits> _queue;
};

// [신규, 2026-09-19, PN-0AC554C2 1단계, QU-25E1C297 답변] `task->
// blockedOn` 리스트를 순회해 `isCompleted()==true`인 엔트리와 이미
// 대상이 해제된(expired) 엔트리를 제거하고, 순회 후에도 리스트가
// 비어있지 않으면 true를 반환한다 - `Scheduler::onTick()`의 재스케줄
// 결정 지점(`kCheckAndMarkFrozen`/`kIsPausedByDebugger`와 같은 자리)이
// 이 반환값을 세 번째 조건으로 사용한다. `kCheckAndMarkFrozen()`과
// 동일한 이유로 **부수 효과(리스트 드레인)가 있으니 단락 평가로
// 건너뛰면 안 된다** - 호출부는 반드시 매번 이 함수를 부른 뒤 결과를
// 확인해야 한다.
bool kDrainAndCheckBlockedOn(Task* task);

}  // namespace kernel

#endif  // MINICORE_KERNEL_WAIT_QUEUE_H
