#include "timer.h"

#include "x86_64/io_port.h"
#include "acpi.h"
#include "hpet.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "ioapic.h"
#include "lapic.h"
#include "libkenv/types.h"

namespace {

constexpr kernel::uint16_t kPitChannel0Data = 0x40;
constexpr kernel::uint16_t kPitCommand = 0x43;
constexpr kernel::uint32_t kPitFrequencyHz = 1193182;
constexpr kernel::uint32_t kLegacyPitTargetHz = 100;  // 다른 시간원과 같은 틱 레이트

// isr.S의 33-254 동적 벡터 대역 중 하나 - HPET(0x22)과도 안 겹치게
// 골랐다.
constexpr kernel::uint32_t kLegacyPitVector = 0x23;
constexpr kernel::uint32_t kIsaIrqPit = 0;

kernel::uint64_t gTickCount = 0;
bool gUsesHpet = false;
bool gLegacyPitIrqEnabled = false;

void kLegacyPitIrqHandler(kernel::InterruptFrame*) {
    kernel::Timer::onTick();
}

}  // namespace

namespace kernel {

void Timer::init() {
    if (Acpi::hasHpet() && Hpet::init()) {
        gUsesHpet = true;
        return;  // HPET가 스케줄러 틱을 담당 - 아래 폴백 경로는 불필요
    }

    // HPET가 없는 폴백 환경(DC-0CC88ABB/QU-3218B790 설계자 답변 (a),
    // 2026-09-14로 확정) - 물리 LAPIC 주기 타이머는 코어당 하나뿐이라
    // 여기서 별도로 Lapic::startPeriodicTimer(kTimerVector, ...)를
    // 부르면 Scheduler::startTickOnThisCore()가 이미 이 코어(BSP)에
    // 걸어 둔 kSchedulerTickVector 프로그래밍을 덮어써 스케줄러 틱
    // 자체가 끊긴다(실측 전 리뷰로 확인한 잠재 버그 - 이전 코드가
    // 그랬다). 그래서 여기서는 아무 하드웨어도 새로 건드리지 않고,
    // 대신 Scheduler::onTick()이 BSP 코어에서 이 함수 대신
    // Timer::onTick()을 호출해 전역 시각을 공급한다(gUsesHpet==false
    // 를 그 판단 기준으로 그대로 재사용). 그래도 부족하면(예: LAPIC
    // 자체가 신뢰 못 할 하드웨어) enableLegacyPitIrq()로 완전히
    // 독립적인 세 번째 시간원을 수동으로 켤 수 있다.
}

uint64_t Timer::tickCount() {
    return gTickCount;
}

bool Timer::usesHpet() {
    return gUsesHpet;
}

void Timer::onTick() {
    ++gTickCount;
}

bool Timer::enableLegacyPitIrq() {
    if (gLegacyPitIrqEnabled) {
        return true;
    }

    // 채널0을 모드3(사각파)로 kLegacyPitTargetHz 주기로 재프로그램
    // 한다 - 채널2(보정용, 원샷)와는 독립된 채널이라 LAPIC/HPET 경로에
    // 영향 없다.
    const uint32_t divisor = kPitFrequencyHz / kLegacyPitTargetHz;
    arch::kOutB(kPitCommand, 0x36);  // 채널0, lobyte/hibyte, 모드3, 바이너리
    arch::kOutB(kPitChannel0Data, static_cast<uint8_t>(divisor & 0xFF));
    arch::kOutB(kPitChannel0Data, static_cast<uint8_t>((divisor >> 8) & 0xFF));

    Idt::registerHandler(kLegacyPitVector, kLegacyPitIrqHandler);
    const bool routed = IoApic::setRedirectionForIsaIrq(kIsaIrqPit, kLegacyPitVector, Lapic::id());
    if (!routed) {
        Idt::unregisterHandler(kLegacyPitVector);
        return false;
    }

    gLegacyPitIrqEnabled = true;
    return true;
}

void Timer::disableLegacyPitIrq() {
    if (!gLegacyPitIrqEnabled) {
        return;
    }
    const Acpi::IsaIrqRouting routing = Acpi::resolveIsaIrq(kIsaIrqPit);
    IoApic::mask(routing.gsi);
    Idt::unregisterHandler(kLegacyPitVector);
    gLegacyPitIrqEnabled = false;
}

bool Timer::isLegacyPitIrqEnabled() {
    return gLegacyPitIrqEnabled;
}

}  // namespace kernel
