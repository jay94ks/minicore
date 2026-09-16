#include "nmi.h"

#include "acpi.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "scheduler.h"

namespace {

// gAcpiMaxCpus개 고정 배열 - AsyncCallbackRegistry/EventTopicRegistry와
// 같은 이 코드베이스의 표준 "코어별 슬롯" 패턴(TlbShootdown의
// gPendingMask와 동일한 발상, nmi.h 문서 주석 참고).
kernel::AtomicU32 gNmiReason[kernel::kAcpiMaxCpus];

}  // namespace

namespace kernel {

void Nmi::send(uint32_t targetCoreIndex, NmiReason reason) {
    gNmiReason[targetCoreIndex].store(static_cast<uint32_t>(reason));
    Lapic::sendNmiIpi(Acpi::cpuApicId(targetCoreIndex));
}

NmiReason Nmi::reasonForThisCore() {
    return static_cast<NmiReason>(gNmiReason[Scheduler::currentCoreIndex()].load());
}

void Nmi::stopAllOtherCores() {
    const uint32_t selfIndex = Scheduler::currentCoreIndex();
    const uint32_t cpuCount = Acpi::cpuCount();
    for (uint32_t i = 0; i < cpuCount; ++i) {
        if (i == selfIndex) {
            continue;
        }
        send(i, NmiReason::DebugHalt);
    }
}

}  // namespace kernel
