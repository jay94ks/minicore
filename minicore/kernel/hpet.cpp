#include "hpet.h"

#include "acpi.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "ioapic.h"
#include "lapic.h"
#include "paging.h"
#include "timer.h"

namespace {

// LAPIC(kLapicVirtBase)/IOAPIC(kLapicVirtBase+0x1000) 다음 4KiB
// 페이지 - 셋 다 서로 다른 물리 프레임을 가리키는 독립된 MMIO 매핑.
constexpr unsigned long kHpetVirtBase = 0xFFFF901000002000UL;

constexpr unsigned int kRegGeneralCapabilities = 0x000;  // 64비트, 읽기전용
constexpr unsigned int kRegGeneralConfig = 0x010;        // 64비트
constexpr unsigned int kRegTimer0Config = 0x100;         // 64비트
constexpr unsigned int kRegTimer0Comparator = 0x108;     // 64비트

constexpr unsigned long kGeneralConfigEnableBit = 1UL << 0;
constexpr unsigned long kGeneralConfigLegacyRouteBit = 1UL << 1;

constexpr unsigned long kTimerConfigIntEnableBit = 1UL << 2;
constexpr unsigned long kTimerConfigPeriodicBit = 1UL << 3;
constexpr unsigned long kTimerConfigPeriodicCapableBit = 1UL << 4;
constexpr unsigned long kTimerConfigValSetBit = 1UL << 6;

constexpr unsigned long kFemtosecondsPerSecond = 1000000000000000UL;
constexpr unsigned int kTargetHz = 100;  // timer.cpp의 PIT/LAPIC 경로와 같은 틱 레이트

unsigned long gHpetVirtAddr = 0;

unsigned long kReadReg(unsigned int offset) {
    return *reinterpret_cast<volatile unsigned long*>(gHpetVirtAddr + offset);
}

void kWriteReg(unsigned int offset, unsigned long value) {
    *reinterpret_cast<volatile unsigned long*>(gHpetVirtAddr + offset) = value;
}

void kHpetInterruptHandler(kernel::InterruptFrame*) {
    kernel::Timer::onTick();
}

}  // namespace

namespace kernel {

bool Hpet::init() {
    const unsigned long phys = Acpi::hpetAddress();
    Paging::mapPage(kHpetVirtBase, phys, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
    gHpetVirtAddr = kHpetVirtBase;

    const unsigned long caps = kReadReg(kRegGeneralCapabilities);
    const auto periodFemtoseconds = static_cast<unsigned int>(caps >> 32);
    if (periodFemtoseconds == 0) {
        return false;  // 잘못된/에뮬레이션 안 된 HPET - 0으로 나누기 방지
    }

    const unsigned long timer0Config = kReadReg(kRegTimer0Config);
    if (!(timer0Config & kTimerConfigPeriodicCapableBit)) {
        return false;  // Timer0이 주기 모드를 지원 안 함 - 호출부가 LAPIC/PIT로 폴백해야 함
    }

    const unsigned long ticksPerInterval = (kFemtosecondsPerSecond / kTargetHz) / periodFemtoseconds;

    // 주기 모드 설정(HPET 스펙 2.3.9.2.1) - TIMER_VAL_SET_CNF를 세운
    // 채로 comparator에 값을 두 번 연속 써야 한다: 첫 번째는 "다음
    // 인터럽트까지 남은 값"(초기값), 두 번째가 실제 주기(period)로
    // 확정된다(하드웨어가 TIMER_VAL_SET_CNF를 그 두 번째 쓰기 직후
    // 자동으로 지운다) - 초기 지연과 주기를 같은 값으로 두면 그냥
    // 같은 값을 두 번 쓰면 된다. 이 두 번 쓰기를 빼먹으면(한 번만
    // 쓰면) 실제 하드웨어/일부 에뮬레이터에서 주기가 안 걸리고 한 번
    // 쏘고 멈추는 것처럼 보일 수 있다 - 관계도에 기록.
    unsigned long config = timer0Config;
    config |= kTimerConfigIntEnableBit | kTimerConfigPeriodicBit | kTimerConfigValSetBit;
    kWriteReg(kRegTimer0Config, config);
    kWriteReg(kRegTimer0Comparator, ticksPerInterval);
    kWriteReg(kRegTimer0Comparator, ticksPerInterval);

    // Legacy Replacement Route를 켜면 Timer0의 인터럽트가 하드웨어
    // 스펙상 GSI2로 고정 라우팅된다(레거시 PIT/IRQ0 자리) - 그래도
    // IOAPIC 리다이렉션 테이블 엔트리는 우리가 직접 채워야 실제로
    // CPU 벡터까지 전달된다(IOAPIC은 HPET의 내부 라우팅 결정을 알
    // 방법이 없다 - 그냥 GSI2 핀에 뭔가 신호가 온 것으로만 본다).
    Idt::registerHandler(kHpetVector, kHpetInterruptHandler);
    IoApic::setRedirection(2, kHpetVector, Lapic::id());

    unsigned long generalConfig = kReadReg(kRegGeneralConfig);
    generalConfig |= kGeneralConfigLegacyRouteBit | kGeneralConfigEnableBit;
    kWriteReg(kRegGeneralConfig, generalConfig);

    return true;
}

}  // namespace kernel
