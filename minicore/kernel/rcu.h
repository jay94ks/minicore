#ifndef MINICORE_KERNEL_RCU_H
#define MINICORE_KERNEL_RCU_H

#include "libkcont/intrusive_list.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "percpu.h"
#include "scheduler.h"

// RCU(Read-Copy-Update) 인프라 v1(SP-B1E258D8 §5, PN-495C11B7) - 고전
// (비선점) RCU. 이 커널이 이미 "선점 비활성화 구간은 실제로 선점되지
// 않는다"를 보장하므로(PL-2D3184BC §8-부분), quiescent state를 "이
// 코어의 선점 비활성화 카운터가 0으로 돌아온 순간"으로 재해석해
// 새 read-side 프리미티브를 만들지 않는다(§5.1) - 새로 필요한 건
// grace-period 상태 머신(§5.2)과 call_rcu 콜백 큐(§5.3)뿐이다.

namespace kernel {

using RcuGraceperiodSeq = uint64_t;

// rcu_read_lock()/rcu_read_unlock()에 대응 - 기존 disablePreemption()/
// enablePreemption()을 그대로 감싼다(§5.1, PreemptionGuard와 동일한
// 모양이지만 "RCU로 보호되는 구조를 읽는다"는 의도를 이름으로 드러내는
// 것이 유일한 차이). 이 구간 안에서 yield()/블로킹 호출을 하면 안
// 된다는 제약은 PreemptionGuard와 동일(새로 생기는 제약이 아님).
class RcuReadGuard {
public:
    RcuReadGuard() { Scheduler::disablePreemption(); }
    ~RcuReadGuard() { Scheduler::enablePreemption(); }

    RcuReadGuard(const RcuReadGuard&) = delete;
    RcuReadGuard& operator=(const RcuReadGuard&) = delete;
};

// call_rcu 콜백 - 회수 대상 객체에 내장(intrusive)해 별도 Slab 할당을
// 피우는 것을 권장한다(Linux rcu_head 관례와 동일, §5.3) - `link`가
// 그 내장 지점이다. libkcont(SP-FAF768AB)의 `Node`/`List<T, Traits>`를
// 그대로 재사용한다(손짜기 연결 리스트를 새로 만들지 않음).
struct RcuCallback {
    void (*fn)(RcuCallback* self) = nullptr;
    Node link;
    RcuGraceperiodSeq targetSeq = 0;  // 이 유예 기간이 끝나야 실행 가능
};

struct RcuCallbackTraits {
    static constexpr Node RcuCallback::* Link = &RcuCallback::link;
};

// grace-period 상태 머신(§5.2) + call_rcu 콜백 큐(§5.3).
class Rcu {
public:
    // 부팅 시 코어별로 한 번(BSP는 kmain.cpp, AP는 kApMain) -
    // lastObservedSeq를 명시적으로 0으로 초기화한다(PerCpu<AtomicU64>의
    // 정적 zero-init과 이미 같은 값이지만, Resurrect류 재사용이 없는
    // 이 필드에도 다른 boot-once 초기화들과 동일한 명시적 관례를
    // 맞춘다 - RM-23F4B687 §4).
    static void initOnThisCore() { _lastObservedSeq.get().store(0); }

    // 이 코어가 방금 고요 상태를 지났음을 기록한다 - 이 시점까지
    // 유효했던 모든 grace period 요청을 "이 코어는 통과했다"고
    // 표시하는 것과 같으므로, 그 시점의 전역 시퀀스 번호를 그대로
    // 적어 둔다. `Scheduler::enablePreemption()`이 카운터를 0으로
    // 되돌리는 지점에서 내부적으로 호출한다(§5.1 - 호출부가 직접
    // 부를 필요 없음).
    static void noteQuiescentStateOnThisCore() { _lastObservedSeq.get().store(_currentSeq.load()); }

    // 새 유예 기간을 시작하고 그 시퀀스 번호를 반환한다(트리거만 -
    // 완료를 기다리지 않는다).
    static RcuGraceperiodSeq startGracePeriod() { return _currentSeq.fetchAdd(1) + 1; }

    // seq로 시작된 유예 기간이 끝났는지 - 모든 온라인 코어가 그
    // 시점 이후 최소 한 번 고요 상태를 지났는지를 선형 스캔으로
    // 확인한다(§5.2 - 이 커널 규모의 코어 수 전제하에 단순 구현,
    // PerCpu<T>::get()과 동일한 성격의 트레이드오프). 온라인 코어
    // 열거는 `Smp::isCoreOnline()`(PN-907C5289가 이미 확립한 "아직
    // SIPI 트램폴린 중인 코어는 제외" 판정)을 재사용한다.
    static bool isGracePeriodComplete(RcuGraceperiodSeq seq);

    // 이 콜백을 "다음 유예 기간이 끝나면" 실행되도록 이 코어의 콜백
    // 큐에 등록한다 - 즉시 회수(Slab::free 등) 대신 회수를 미루고
    // 싶은 모든 지점에서 쓴다. 내부적으로 새 grace period를 시작해
    // targetSeq를 채운다.
    static void callAfterGracePeriod(RcuCallback* cb);

    // AsyncReactor::drainOnce(coreIndex)가 매 호출마다(§5.3, "리액터
    // 컨텍스트에서만 콜백 실행" 원칙) 이 코어의 pending 리스트를
    // 앞에서부터 훑어 이미 완료된 유예 기간의 콜백만 떼어내 실행한다
    // - 완료 안 된 항목을 만나면 그 뒤는 더 최근에 등록된 것들이므로
    // (pushBack이 등록 순서를 유지) 순회를 멈춘다.
    static void drainCallbacksOnThisCore();

private:
    static PerCpu<AtomicU64> _lastObservedSeq;
    static AtomicU64 _currentSeq;
    static PerCpu<List<RcuCallback, RcuCallbackTraits>> _pendingList;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_RCU_H
