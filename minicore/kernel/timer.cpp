#include "timer.h"

#include "x86_64/io_port.h"
#include "acpi.h"
#include "hpet.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "ioapic.h"
#include "lapic.h"

namespace {

constexpr unsigned short kPitChannel0Data = 0x40;
constexpr unsigned short kPitCommand = 0x43;
constexpr unsigned int kPitFrequencyHz = 1193182;
constexpr unsigned int kTimerTargetHz = 100;
constexpr unsigned int kLegacyPitTargetHz = 100;  // 다른 시간원과 같은 틱 레이트

// isr.S의 33-254 동적 벡터 대역 중 하나 - HPET(0x22)과도 안 겹치게
// 골랐다.
constexpr unsigned int kLegacyPitVector = 0x23;
constexpr unsigned int kIsaIrqPit = 0;

unsigned long gTickCount = 0;
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
        return;  // HPET가 스케줄러 틱을 담당 - 아래 PIT 보정/LAPIC 주기 설정은 불필요
    }

    Lapic::startPeriodicTimer(kTimerVector, kTimerTargetHz);
}

unsigned long Timer::tickCount() {
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
    const unsigned int divisor = kPitFrequencyHz / kLegacyPitTargetHz;
    arch::kOutB(kPitCommand, 0x36);  // 채널0, lobyte/hibyte, 모드3, 바이너리
    arch::kOutB(kPitChannel0Data, static_cast<unsigned char>(divisor & 0xFF));
    arch::kOutB(kPitChannel0Data, static_cast<unsigned char>((divisor >> 8) & 0xFF));

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
