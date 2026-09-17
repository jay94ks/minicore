#ifndef MINICORE_KERNEL_CONCURRENT_RBTREE_H
#define MINICORE_KERNEL_CONCURRENT_RBTREE_H

#include "libkcont/rbtree.h"
#include "libkenv/spinlock.h"
#include "rcu.h"

// [배치 정정] ConcurrentMap(concurrent_map.h)과 동일한 이유로
// `minicore/libs/libkcont`가 아니라 여기(`minicore/kernel`)에 둔다 -
// 정확성이 RCU(호출부의 유예 반납 계약, 아래 참고)와 결부되는데 RCU는
// Scheduler/PerCpu 기반 순수 커널 개념이라 유저랜드 대응물이 없다.
//
// ConcurrentRbtree<T, Traits>(SP-4DCD0E6A §3, PN-A8EF29F7) -
// `Rbtree<T, Traits>`(libkcont)를 그대로 감싸는 wrapper: 쓰기(insert/
// remove)는 단일 전역 `Spinlock`으로 직렬화하고, 읽기(find/first/next)
// 는 완전히 락 없이(lock-free) `Rbtree`를 그대로 호출한다.
//
// **[결정, 2026-09-17, QU-B5CA4008 설계자 답변 - "(A) 읽기
// seqlock류 검증/재시도"]** 착수 전 코드 감사에서 `Rbtree`가 공유하는
// `detail::RbCore::rotateLeft/rotateRight`(rbtree.h)가 CLRS 표준
// 그대로 여러 필드를 순차적으로 mutate하는 in-place 회전이라(단일
// 원자적 포인터 교체가 아님) 발견됐다 - 락 없이 순회 중인 리더가
// 회전 도중의 일시적으로 비일관된 포인터 그래프를 볼 수 있다는
// 우려가 실제로 확인됐다. 두 방향(읽기 쪽 seqlock 재시도 / RbCore
// 회전 로직 자체를 원자적 교체 방식으로 재설계) 중 설계자가 전자를
// 택했다 - `Rbtree`/`RbMultiTree`의 기존 비동시 소비자에게 전혀
// 영향을 주지 않는 국소적 변경이라는 이유.
//
// **구현**: 버전 카운터(`_version`, 짝수=고요/홀수=쓰기 진행 중,
// 고전적 seqlock 관례)를 쓰기 시작 직전 홀수로, 끝난 직후 짝수로
// 만든다. 읽기는 시작 직전 버전을 기록해 두고 `Rbtree`의 원래
// (수정 안 한) `find()`/`first()`/`next()`를 그대로 호출한 뒤, 끝난
// 시점의 버전이 시작 시점과 같으면(=짝수 그대로 유지) 그 결과를
// 신뢰하고, 다르면(쓰기가 끼어들었음) 처음부터 재시도한다 - 설계자
// 답변이 명시한 그대로 "순회 전후 버전을 비교"하는 방식이다.
//
// **[정직하게 기록해 둘 잔여 위험]** 이 seqlock 패턴은 "순회가 끝난
// 뒤" 변경 여부만 감지한다 - 순회가 진행되는 바로 그 순간에 회전이
// 끼어들면(예: `rotateLeft`의 `x->right = y->left` 실행 직후, `y->left
// = x` 실행 직전 사이의 극히 짧은 창에서 리더가 마침 x와 y 둘 다를
// 지나는 중이면) 이론적으로 리더가 일시적 순환(x->right가 y, y->left가
// x)을 밟아 이번 시도 자체가 끝나지 않을 수 있다 - 재시도 카운터
// (`kMaxRetries`)로 그 경우에도 무한 대기는 피하지만, **그 한 번의
// 시도 자체가 되돌아오지 못하는 이론적 가능성 자체를 완전히 제거하지는
// 않는다**(설계자가 택한 (A) 방향 자체가 감내하기로 한 절충 - 완전한
// 제거는 (B) RbCore 재설계가 필요했음, QU-B5CA4008 참고). 실사용
// 패턴(쓰기가 드물고 회전이 극히 짧음)에서는 발동 확률이 매우 낮다고
// 판단하지만, 실제로 관측되면 이 사실을 근거로 (B) 재검토를 요청할
// 근거로 남겨 둔다.
//
// **RCU 계약(caller 책임)**: `Rbtree`와 마찬가지로 이 컨테이너는 T의
// 메모리를 전혀 소유/해제하지 않는다(완전히 침습적) - `remove(item)`이
// 반환한 뒤에도 다른 코어가 `find()`/`first()`/`next()`로 그 `item`의
// `RbNode`(포인터 필드)를 통해 아직 순회 중일 수 있다(`LockFreeList`와
// 동일한 이유) - 호출부는 `remove()` 직후 즉시 `item`을 재사용/해제
// 하지 말고 자신의 `RcuCallback`으로 `Rcu::callAfterGracePeriod()`를
// 거쳐야 한다.
namespace kernel {

template <typename T, typename Traits>
class ConcurrentRbtree {
public:
    using Key = typename Traits::Key;

    bool insert(T* item) {
        SpinlockGuard guard(_writeLock);
        _version.fetchAdd(1);  // 홀수 - 쓰기 시작
        const bool ok = _tree.insert(item);
        _version.fetchAdd(1);  // 짝수 - 쓰기 끝
        return ok;
    }

    void remove(T* item) {
        SpinlockGuard guard(_writeLock);
        _version.fetchAdd(1);
        _tree.remove(item);
        _version.fetchAdd(1);
    }

    T* find(const Key& key) const {
        return kRetryRead([&]() { return _tree.find(key); });
    }

    T* first() const {
        return kRetryRead([&]() { return _tree.first(); });
    }

    T* next(T* item) const {
        return kRetryRead([&]() { return _tree.next(item); });
    }

    bool empty() const { return _tree.empty(); }

private:
    // 쓰기 도중(홀수)이 아닌 짝수 버전을 하나 얻어 그 값을 기준으로
    // op()을 실행하고, 끝난 뒤 버전이 그대로면 결과를 신뢰한다 - 위
    // 클래스 문서 주석의 seqlock 패턴 그대로. `RcuReadGuard`로 이
    // 코어의 선점까지 막아 둔다(다른 lock-free 컨테이너들과 동일한
    // 관례 - 최소한 "이 코어 자신이 읽기 도중 다른 Task로 전환됐다가
    // 한참 뒤에 재개되는" 종류의 극단적으로 긴 창은 원천 차단).
    template <typename Op>
    T* kRetryRead(Op&& op) const {
        constexpr uint32_t kMaxRetries = 64;
        for (uint32_t attempt = 0; attempt < kMaxRetries; ++attempt) {
            RcuReadGuard guard;
            uint64_t before = _version.load();
            if (before & 1) {
                continue;  // 쓰기 진행 중 - 곧바로 재시도
            }
            T* result = op();
            const uint64_t after = _version.load();
            if (before == after) {
                return result;
            }
        }
        // [정직하게 기록] kMaxRetries를 전부 소진 - 쓰기가 지속적으로
        // 끼어들었거나(livelock) 위 "잔여 위험" 절의 극히 드문 경우.
        // 마지막으로 한 번 더(재시도 없이) 결과를 그대로 반환한다 -
        // 완전한 정확성보다 무한 대기를 피하는 쪽을 택함(RM-23F4B687
        // §4 - 커널 코드는 절대 멈추면 안 된다는 원칙이 우선).
        RcuReadGuard guard;
        return op();
    }

    Rbtree<T, Traits> _tree;
    Spinlock _writeLock;
    AtomicU64 _version{0};
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_CONCURRENT_RBTREE_H
