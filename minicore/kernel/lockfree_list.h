#ifndef MINICORE_KERNEL_LOCKFREE_LIST_H
#define MINICORE_KERNEL_LOCKFREE_LIST_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "rcu.h"

// [배치 정정, 2026-09-17] SP-4DCD0E6A는 이 컨테이너를 `libkcont`(커널/
// 유저 공용)에 두도록 스케치했으나, 실제 착수해 보니 정확성 자체가
// RCU(`rcu.h`)에 의존하고 RCU는 `Scheduler`/`PerCpu`(코어별 선점
// 비활성화 카운터)를 쓰는 순수 커널 개념이라 유저랜드에 대응물이
// 전혀 없다 - `libkcont`가 이 헤더를 담으면 "커널/유저 공용" 헤더가
// 커널 전용 헤더(`rcu.h`)를 include하는 계층 위반이 된다(실제로
// 컴파일도 안 됨 - `rcu.h`는 `minicore/kernel`에 있어 `libkcont`의
// include 경로에서 안 보임). `GenericSlabAllocator`를 `libkcont`가
// 몰라야 해서 `KernelVectorPolicy`를 `minicore/kernel/
// kernel_container_policy.h`에 따로 둔 것과 정확히 같은 이유로,
// 이 헤더도 `minicore/kernel`에 둔다 - 설계 자체(알고리즘/API)는
// 전혀 안 바뀌었고 파일 위치만 조정했다(RM-23F4B687 디렉터리 배치
// 규칙 - PN-633BF2D8이 5종 컨테이너를 한 파일로 합친 것과 같은 급의
// 구현 세부 판단, 별도 설계자 확인 불필요).
//
// LockFreeList<T, Traits>(SP-4DCD0E6A §1, PN-013215F9, Harris 2001) -
// 침습적(intrusive) 단일 연결 lock-free 정렬 집합
// (오름차순, 유일 키 - Rbtree/Map과 같은 "중복 키 거부" 관례). 포인터
// 최하위 비트 1개를 "논리적으로 삭제됨" 마크로 재사용한다(포인터는
// 8바이트 정렬이라 하위 비트가 항상 0 - Maple Tree(SP-2AAD7C8D)가
// 이미 쓴 포인터 태깅과 같은 기법). 삭제는 "마크만 세우기(CAS 1회,
// `remove()`가 예측 가능한 O(1))" + "물리적 unlink는 다음 순회가
// 지나가며 청소(헬핑)"의 2단계로 분리한다.
//
// **RCU 필요 이유(SP-B1E258D8/PN-495C11B7)**: `remove(item)`은 `item`
// 자신을 논리적으로 마킹만 할 뿐, 그 메모리를 이 컨테이너가 절대
// 해제하지 않는다(비-lock-free `List<T,Traits>`와 동일하게 T의
// 메모리는 항상 호출자 소유) - 하지만 마킹 직후 다른 스레드가 아직
// 그 노드를 헬핑 순회 중일 수 있으므로, **호출자는 `remove()`가
// 반환한 뒤에도 실제로 `item`을 재사용/해제하기 전에 반드시 자기
// 자신의 회수를 `Rcu::callAfterGracePeriod()`로 미뤄야 한다**(이
// 컨테이너는 그 스케줄링까지 대신 해 주지 않는다 - T의 실제 회수
// 방법을 이 제네릭 컨테이너가 알 수 없으므로, 그 결정은 항상 호출자
// 몫이라는 libkcont 전역 원칙과 동일). `find()`/`insert()`의 순회
// 자체도 `RcuReadGuard`로 감싸 grace-period 계산이 이 코어를 올바르게
// "아직 읽는 중"으로 취급하게 한다 - 이게 없으면 다른 코어가 이
// 스레드가 아직 순회 중인 사이에도 grace period가 끝났다고 착각할
// 수 있다.
namespace kernel {

struct LockFreeNode {
    AtomicPtr<LockFreeNode> next;  // 최하위 비트 = 논리적 삭제 마크
};

template <typename T, typename Traits>
class LockFreeList {
public:
    using Key = typename Traits::Key;

    void init() { _head.next.store(nullptr); }

    // 실패(이미 같은 키 존재) 시 false.
    bool insert(T* item) {
        RcuReadGuard guard;
        const Key key = Traits::keyOf(*item);
        LockFreeNode* node = &(item->*Traits::Link);
        for (;;) {
            LockFreeNode* pred = nullptr;
            LockFreeNode* curr = nullptr;
            if (kSearch(key, &pred, &curr)) {
                return false;  // 이미 존재(유일 키 집합, Rbtree/Map과 동일 관례)
            }
            node->next.store(curr);
            if (pred->next.compareExchange(curr, node)) {
                return true;
            }
            // pred->next가 그 사이 바뀜(다른 스레드의 삽입/헬핑) - 처음부터 재검색.
        }
    }

    T* find(const Key& key) const {
        RcuReadGuard guard;
        LockFreeNode* pred = nullptr;
        LockFreeNode* curr = nullptr;
        if (kSearch(key, &pred, &curr)) {
            return kContainerOf(curr);
        }
        return nullptr;
    }

    // 논리적 마킹만(위 클래스 문서 주석의 RCU 계약 참고) - 이미
    // 마킹돼 있으면(중복 remove) false.
    static bool remove(T* item) {
        RcuReadGuard guard;
        LockFreeNode* node = &(item->*Traits::Link);
        for (;;) {
            LockFreeNode* next = node->next.load();
            if (kIsMarked(next)) {
                return false;
            }
            if (node->next.compareExchange(next, kMark(next))) {
                return true;
            }
        }
    }

private:
    static bool kIsMarked(LockFreeNode* ptr) { return (reinterpret_cast<uint64_t>(ptr) & 1ULL) != 0; }
    static LockFreeNode* kUnmark(LockFreeNode* ptr) {
        return reinterpret_cast<LockFreeNode*>(reinterpret_cast<uint64_t>(ptr) & ~1ULL);
    }
    static LockFreeNode* kMark(LockFreeNode* ptr) {
        return reinterpret_cast<LockFreeNode*>(reinterpret_cast<uint64_t>(ptr) | 1ULL);
    }

    // Node* -> T* : List<T,Traits>(intrusive_list.h)의 kContainerOf와
    // 완전히 같은 관용구(offsetof 매크로 대신 멤버 포인터 오프셋 -
    // non-standard-layout 안전).
    static T* kContainerOf(LockFreeNode* node) {
        const uint64_t offset = reinterpret_cast<uint64_t>(&(reinterpret_cast<T*>(0)->*Traits::Link));
        return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(node) - offset);
    }

    // pred/curr를 채운다. 반환값 true면 curr가 정확히 그 key를 가진
    // 노드(찾음) - false면 curr는 그 key가 삽입돼야 할 위치의 다음
    // 노드(또는 리스트 끝이면 nullptr), pred는 그 직전 노드. 지나는
    // 길에 마킹된(논리 삭제된) 노드를 만나면 CAS로 물리적으로
    // unlink한다(Harris "헬핑") - 그 CAS가 경합으로 실패하면 head부터
    // 처음부터 다시 훑는다(원 논문의 표준 재시도 전략).
    bool kSearch(const Key& key, LockFreeNode** outPred, LockFreeNode** outCurr) const {
        LockFreeNode* pred = const_cast<LockFreeNode*>(&_head);
        LockFreeNode* curr = kUnmark(pred->next.load());
        for (;;) {
            if (!curr) {
                *outPred = pred;
                *outCurr = nullptr;
                return false;
            }
            LockFreeNode* succ = curr->next.load();
            if (kIsMarked(succ)) {
                LockFreeNode* expected = curr;
                if (!pred->next.compareExchange(expected, kUnmark(succ))) {
                    // 경합(다른 스레드가 그 사이 pred->next를 바꿈) -
                    // Harris 원 논문의 표준 재시도 전략대로 head부터
                    // 다시 훑는다.
                    pred = const_cast<LockFreeNode*>(&_head);
                    curr = kUnmark(pred->next.load());
                    continue;
                }
                curr = kUnmark(succ);
                continue;
            }
            const Key currKey = Traits::keyOf(*kContainerOf(curr));
            if (currKey < key) {
                pred = curr;
                curr = succ;  // succ는 여기서 이미 unmarked임이 확인됨
                continue;
            }
            *outPred = pred;
            *outCurr = curr;
            return !(key < currKey);  // !(key < currKey) && !(currKey < key) => 같음
        }
    }

    LockFreeNode _head;  // sentinel - next 기본값 nullptr(별도 init() 없이도 안전)
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_LOCKFREE_LIST_H
