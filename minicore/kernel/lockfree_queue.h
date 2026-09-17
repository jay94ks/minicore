#ifndef MINICORE_KERNEL_LOCKFREE_QUEUE_H
#define MINICORE_KERNEL_LOCKFREE_QUEUE_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "rcu.h"

// [배치 정정, 2026-09-17] `lockfree_list.h`와 같은 이유 - RCU에 의존해
// 정확성이 성립하는데 RCU는 순수 커널 개념(Scheduler/PerCpu 기반)이라
// 유저랜드 대응물이 없다. SP-4DCD0E6A는 `libkcont`(커널/유저 공용)에
// 두도록 스케치했지만, `libkcont`가 커널 전용 `rcu.h`를 include하면
// 계층 위반(및 컴파일 실패 - `libkcont`의 include 경로에 `rcu.h`가
// 없음)이라 `GenericSlabAllocator`/`KernelVectorPolicy`와 동일한 이유로
// `minicore/kernel`에 둔다 - 설계(알고리즘/API)는 그대로, 파일 위치만
// 조정(구현 세부 판단, 별도 설계자 확인 불필요).
//
// LockFreeQueue<T, Traits>(SP-4DCD0E6A §4, PN-013215F9, Michael-Scott
// 1996) - MPMC lock-free FIFO 큐.
//
// **[결정, 2026-09-17, QU-253D6D24 설계자 답변] 비침습(non-intrusive)
// 설계**: 최초 스케치는 T 안에 LockFreeNode를 내장하는 침습적
// 설계였으나, 착수 전 자가 점검(RM-32D06563 "예약 먼저, 자원 할당
// 나중" 원칙과는 별개의 새 발견)에서 구조적 결함이 드러났다 - Michael-
// Scott 원 알고리즘은 dequeue가 반환하는 값과 새로 "더미"가 되는
// 노드가 서로 다른 메모리(Node/Value 분리)임을 전제하는데, 침습
// 설계는 그 둘이 같은 메모리가 돼 호출자가 반환받은 T를 정상적으로
// 재사용/해제하면 큐 내부 상태가 손상된다. 그래서 이 큐 전용 별도
// `LockFreeQueueNode<T>` 래퍼(`T*`만 담음)로 바꿨다 - `T` 자신은
// 호출자에게 소유권이 넘어가는 즉시 완전히 자유롭다(더 이상 큐
// 내부 구조와 메모리를 공유하지 않음).
//
// **`enqueue`는 Node 래퍼 할당을 CAS를 시도하기 *전에* 반드시 끝낸다**
// - 실패하면 그 자리에서 false를 반환할 뿐 큐 상태에는 어떤 예약/
// 구멍도 남기지 않는다(`LockFreeVector`(PN-DAE91888) 사후분석이 확립한
// "예약 먼저, 자원 할당 나중은 금지" 원칙(RM-32D06563)의 반례 - 이
// 컨테이너는 처음부터 그 원칙을 지키게 설계됐다).
//
// **RCU 필요 이유**: `dequeue()`가 물러난 옛 head(더미) Node 래퍼는
// 다른 스레드가 아직 그 노드를 통해 tail을 따라가는 중일 수 있어
// 즉시 반납하지 않는다 - `Rcu::callAfterGracePeriod()`로 미룬다.
// `enqueue`/`dequeue`의 순회 자체도 `RcuReadGuard`로 감싼다(이
// 코어가 "아직 읽는 중"임을 grace-period 계산에 알리기 위함 -
// `LockFreeList`와 동일한 이유).
namespace kernel {

template <typename T>
struct LockFreeQueueNode {
    AtomicPtr<LockFreeQueueNode> next;
    T* value = nullptr;
    // 이 노드(래퍼) 자신을 나중에 반납할 때 쓸 할당자 - dequeue가
    // 옛 head를 RCU 유예 콜백(kReclaim, 정적 함수라 LockFreeQueue
    // 인스턴스에 접근할 수 없음)으로 넘길 때, 그 콜백이 어떤 FreeFn을
    // 써야 할지 이 노드 자신에 미리 적어 둔다.
    void (*freeFn)(void* ptr, uint64_t size) = nullptr;
    RcuCallback rcuCb;
};

// `Traits`는 SP-4DCD0E6A §4 원안의 `<T, Traits>` 시그니처를 그대로
// 유지하기 위해 남겨 둔 자리다 - 비침습 설계로 바뀐 뒤로는 Link/Key
// 멤버가 전혀 필요 없어 현재는 실제로 쓰이지 않는다(다른 libkcont
// 컨테이너들과의 API 형태 일관성 유지 목적, 사용하지 않는 파라미터를
// 둬도 컴파일/런타임 비용은 없다).
template <typename T, typename Traits>
class LockFreeQueue {
public:
    using Node = LockFreeQueueNode<T>;
    using AllocFn = void* (*)(uint64_t size);
    using FreeFn = void (*)(void* ptr, uint64_t size);

    // 더미 sentinel도 이 시점에 확보한다 - head==tail==dummy로
    // 시작해야(값 없는 빈 자리 하나) 큐가 절대 완전히 비지 않는다는
    // Michael-Scott 알고리즘의 불변조건이 성립한다. 더미 확보 실패
    // (할당 고갈)면 `_head`가 계속 nullptr로 남아 이후 enqueue/dequeue
    // 둘 다 안전하게 실패(false/nullptr)한다.
    void ensureAllocator(AllocFn allocFn, FreeFn freeFn) {
        if (_alloc != nullptr) {
            return;
        }
        _alloc = allocFn;
        _free = freeFn;
        Node* dummy = static_cast<Node*>(_alloc(sizeof(Node)));
        if (!dummy) {
            return;
        }
        dummy->next.store(nullptr);
        dummy->value = nullptr;
        dummy->freeFn = _free;
        _head.store(dummy);
        _tail.store(dummy);
    }

    // 실패(할당자 미설정, 더미 미확보, 또는 Node 래퍼 할당 실패) 시
    // false - 어느 경우든 큐 상태는 전혀 바뀌지 않는다.
    bool enqueue(T* item) {
        if (!_alloc || !_head.load()) {
            return false;
        }
        Node* node = static_cast<Node*>(_alloc(sizeof(Node)));
        if (!node) {
            return false;  // 구멍 없음 - CAS를 아직 한 번도 시도하지 않음
        }
        node->next.store(nullptr);
        node->value = item;
        node->freeFn = _free;

        RcuReadGuard guard;
        for (;;) {
            Node* tailNode = _tail.load();
            Node* next = tailNode->next.load();
            if (tailNode != _tail.load()) {
                continue;  // tail이 그 사이 바뀜 - 일관성 재확인
            }
            if (!next) {
                if (tailNode->next.compareExchange(next, node)) {
                    _tail.compareExchange(tailNode, node);  // best-effort swing(실패해도 무방 - 다음 호출이 헬핑)
                    return true;
                }
            } else {
                _tail.compareExchange(tailNode, next);  // 다른 스레드가 이미 연결한 노드로 tail 전진 도움
            }
        }
    }

    // 비어 있으면 nullptr.
    T* dequeue() {
        if (!_head.load()) {
            return nullptr;
        }
        RcuReadGuard guard;
        for (;;) {
            Node* headNode = _head.load();
            Node* tailNode = _tail.load();
            Node* next = headNode->next.load();
            if (headNode != _head.load()) {
                continue;
            }
            if (headNode == tailNode) {
                if (!next) {
                    return nullptr;  // 정말 비어 있음
                }
                _tail.compareExchange(tailNode, next);  // 느슨한 tail 전진 도움
                continue;
            }
            T* value = next->value;
            if (_head.compareExchange(headNode, next)) {
                // 옛 head(방금까지의 더미)는 다른 스레드가 아직 그
                // 노드를 통해 tail을 따라가는 중일 수 있어 즉시
                // 반납하지 않는다 - RCU 유예 기간 이후 반납.
                headNode->rcuCb.fn = &kReclaim;
                Rcu::callAfterGracePeriod(&headNode->rcuCb);
                return value;
            }
        }
    }

    bool empty() const {
        Node* headNode = _head.load();
        if (!headNode) {
            return true;
        }
        return headNode == _tail.load() && !headNode->next.load();
    }

private:
    // RcuCallback -> Node : offsetof 매크로 대신 멤버 포인터 오프셋
    // (intrusive_list.h의 kContainerOf와 동일한 관용구 - non-standard-
    // layout 안전).
    static void kReclaim(RcuCallback* cb) {
        const uint64_t offset = reinterpret_cast<uint64_t>(&(reinterpret_cast<Node*>(0)->rcuCb));
        auto* node = reinterpret_cast<Node*>(reinterpret_cast<uint8_t*>(cb) - offset);
        node->freeFn(node, sizeof(Node));
    }

    AllocFn _alloc = nullptr;
    FreeFn _free = nullptr;
    AtomicPtr<Node> _head;
    AtomicPtr<Node> _tail;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_LOCKFREE_QUEUE_H
