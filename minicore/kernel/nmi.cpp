#include "nmi.h"

#include "acpi.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "scheduler.h"
#include "smp.h"

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
    // [수정, 2026-09-17, PN-907C5289 실측 확인] `Acpi::cpuCount()`는
    // ACPI MADT가 보고하는 "이 시스템에 존재하는" 코어 총수일 뿐,
    // 지금 이 순간 실제로 SIPI 트램폴린을 통과해 자기 IDT/장기모드까지
    // 갖춘 "온라인" 코어 수와 다르다 - `Smp::startApCores()`는 AP를
    // 한 번에 하나씩 순차 기동하므로, essential 서비스 사망 같은
    // 패닉이 그 기동 대기 루프 도중(스케줄러 틱 인터럽트로) 발생하면
    // 아직 SIPI조차 못 받았거나 16/32비트 과도기라 유효한 IDT가 없는
    // AP에까지 이 NMI가 도착해 그 코어가 트리플 폴트를 일으킨다(실측
    // 확인 - `git stash`로 이 문제와 무관한 다른 변경을 대조해도 항상
    // 재현). `Smp::isCoreOnline()`(코어별 슬롯, `markThisCoreOnline()`
    // 이 BSP/각 AP 자신의 부팅 완료 시점에 세팅)로 실제 온라인 코어만
    // 골라 보낸다 - 아직 안 뜬 코어는 어차피 공유 상태를 아직 건드리지
    // 않았으므로 stop-the-world 대상에서 빠져도 안전하다.
    for (uint32_t i = 0; i < cpuCount; ++i) {
        if (i == selfIndex || !Smp::isCoreOnline(i)) {
            continue;
        }
        send(i, NmiReason::DebugHalt);
    }
}

}  // namespace kernel
