#include "rcu.h"

#include "acpi.h"
#include "smp.h"

namespace kernel {

PerCpu<AtomicU64> Rcu::_lastObservedSeq;
AtomicU64 Rcu::_currentSeq{0};
PerCpu<List<RcuCallback, RcuCallbackTraits>> Rcu::_pendingList;

bool Rcu::isGracePeriodComplete(RcuGraceperiodSeq seq) {
    const uint32_t coreCount = Acpi::cpuCount();
    for (uint32_t i = 0; i < coreCount; ++i) {
        // [PN-907C5289 교훈 재사용] 아직 SIPI 트램폴린 중인(온라인
        // 아닌) 코어는 애초에 quiescent state를 관찰할 방법이 없다 -
        // 건너뛴다(그 코어가 온라인되면 initOnThisCore()가 0으로
        // 시작하므로, 그 이후 첫 grace period부터는 정상적으로 관찰됨).
        if (!Smp::isCoreOnline(i)) {
            continue;
        }
        if (_lastObservedSeq.forCore(i).load() < seq) {
            return false;
        }
    }
    return true;
}

void Rcu::callAfterGracePeriod(RcuCallback* cb) {
    // [§5.6 열린 하위 과제 2, RM-23F4B687 §4 - 구현 시점 확정] 콜백
    // 등록마다 새 grace period를 시작한다(배치로 묶지 않는 가장 단순한
    // v1 - 나중에 등록될 콜백의 grace period가 먼저 등록된 콜백의
    // grace period보다 항상 뒤에 오므로, drainCallbacksOnThisCore()의
    // "앞에서부터 훑다가 미완료를 만나면 멈춘다" 전제가 그대로 성립).
    cb->targetSeq = startGracePeriod();
    _pendingList.get().pushBack(cb);
}

void Rcu::drainCallbacksOnThisCore() {
    List<RcuCallback, RcuCallbackTraits>& list = _pendingList.get();
    while (!list.empty()) {
        RcuCallback* front = list.front();
        if (!isGracePeriodComplete(front->targetSeq)) {
            break;  // 등록 순서 유지 - 뒤는 더 최근이라 더더욱 미완료
        }
        List<RcuCallback, RcuCallbackTraits>::remove(front);
        front->fn(front);
    }
}

}  // namespace kernel
