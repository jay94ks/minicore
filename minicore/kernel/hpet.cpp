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
constexpr unsigned int kRegGeneralIntStatus = 0x020;     // 64비트 - 레벨 트리거 비교기 ack용
constexpr unsigned int kTimerRegStride = 0x20;
constexpr unsigned int kRegTimer0ConfigBase = 0x100;      // 64비트 - N번은 +0x20*N
constexpr unsigned int kRegTimer0ComparatorBase = 0x108;  // 64비트 - N번은 +0x20*N

constexpr unsigned long kGeneralConfigEnableBit = 1UL << 0;

constexpr unsigned long kTimerConfigTypeLevelBit = 1UL << 1;  // 0=edge, 1=level
constexpr unsigned long kTimerConfigIntEnableBit = 1UL << 2;
constexpr unsigned long kTimerConfigPeriodicBit = 1UL << 3;
constexpr unsigned long kTimerConfigPeriodicCapableBit = 1UL << 4;
constexpr unsigned long kTimerConfigValSetBit = 1UL << 6;
constexpr unsigned int kTimerConfigIntRouteShift = 9;
constexpr unsigned long kTimerConfigIntRouteMask = 0x1FUL << kTimerConfigIntRouteShift;
constexpr unsigned int kTimerConfigRouteCapShift = 32;  // Tn_INT_ROUTE_CAP - 상위 32비트 비트맵

constexpr unsigned int kCapsNumTimShift = 8;
constexpr unsigned int kCapsNumTimMask = 0x1F;  // NUM_TIM_CAP: 실제 타이머 개수 - 1

constexpr unsigned long kFemtosecondsPerSecond = 1000000000000000UL;
constexpr unsigned int kDefaultTargetHz = 100;  // timer.cpp의 PIT/LAPIC 경로와 같은 틱 레이트
constexpr unsigned int kMaxHpetTimers = 32;      // NUM_TIM_CAP 필드가 5비트라 이 이상은 불가능
constexpr unsigned int kInvalidTimerIndex = 0xFFFFFFFFU;

unsigned long gHpetVirtAddr = 0;
unsigned int gTimerCount = 0;
unsigned int gVectorToTimerIndex[256];  // 초기화는 Hpet::init()에서 전부 kInvalidTimerIndex로

unsigned long kReadReg(unsigned int offset) {
    return *reinterpret_cast<volatile unsigned long*>(gHpetVirtAddr + offset);
}

void kWriteReg(unsigned int offset, unsigned long value) {
    *reinterpret_cast<volatile unsigned long*>(gHpetVirtAddr + offset) = value;
}

unsigned int kTimerConfigOffset(unsigned int timerIndex) { return kRegTimer0ConfigBase + timerIndex * kTimerRegStride; }
unsigned int kTimerComparatorOffset(unsigned int timerIndex) {
    return kRegTimer0ComparatorBase + timerIndex * kTimerRegStride;
}

// Tn_INT_ROUTE_CAP 비트맵(이 타이머가 실제로 라우팅 가능한 IOAPIC
// GSI 목록)에서 가장 낮은 번호의 사용 가능한 GSI를 고른다 - 스펙상
// 어떤 특정 GSI를 우선해야 한다는 규정은 없고, 여러 타이머가 겹치는
// GSI를 고르지 않도록 호출부(enableTimer)가 순서대로 타이머를 켤
// 때 자연히 分산되길 기대한다(진짜 충돌 회피까지는 범위 밖).
unsigned int kPickGsiFromRouteCap(unsigned long timerConfig) {
    const auto routeCap = static_cast<unsigned int>(timerConfig >> kTimerConfigRouteCapShift);
    if (routeCap == 0) {
        return kInvalidTimerIndex;
    }
    for (unsigned int gsi = 0; gsi < 32; ++gsi) {
        if (routeCap & (1U << gsi)) {
            return gsi;
        }
    }
    return kInvalidTimerIndex;
}

void kHpetInterruptHandler(kernel::InterruptFrame* frame) {
    kernel::Timer::onTick();
    const unsigned int timerIndex = gVectorToTimerIndex[frame->vector & 0xFF];
    if (timerIndex != kInvalidTimerIndex) {
        // 레벨 트리거 비교기는 General Interrupt Status Register의
        // 해당 비트를 직접 지워야 인터럽트가 계속 걸리지 않는다(엣지
        // 트리거는 이 레지스터를 안 써도 되지만 1을 써도 무해함 -
        // 스펙상 "쓰기 1로 지움"이라 이미 0인 비트에 1을 써도 안전).
        kWriteReg(kRegGeneralIntStatus, 1UL << timerIndex);
    }
}

}  // namespace

namespace kernel {

bool Hpet::init() {
    const unsigned long phys = Acpi::hpetAddress();
    Paging::mapPage(kHpetVirtBase, phys, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
    gHpetVirtAddr = kHpetVirtBase;

    for (unsigned int i = 0; i < 256; ++i) {
        gVectorToTimerIndex[i] = kInvalidTimerIndex;
    }

    const unsigned long caps = kReadReg(kRegGeneralCapabilities);
    gTimerCount = static_cast<unsigned int>((caps >> kCapsNumTimShift) & kCapsNumTimMask) + 1;
    if (gTimerCount > kMaxHpetTimers) {
        gTimerCount = kMaxHpetTimers;
    }

    unsigned long generalConfig = kReadReg(kRegGeneralConfig);
    generalConfig |= kGeneralConfigEnableBit;
    kWriteReg(kRegGeneralConfig, generalConfig);

    return enableTimer(0, kDefaultTargetHz, kHpetVector, Lapic::id());
}

unsigned int Hpet::timerCount() { return gTimerCount; }

bool Hpet::enableTimer(unsigned int timerIndex, unsigned int frequencyHz, unsigned int vector,
                        unsigned int destApicId) {
    if (timerIndex >= gTimerCount || frequencyHz == 0) {
        return false;
    }

    const unsigned long caps = kReadReg(kRegGeneralCapabilities);
    const auto periodFemtoseconds = static_cast<unsigned int>(caps >> 32);
    if (periodFemtoseconds == 0) {
        return false;  // 잘못된/에뮬레이션 안 된 HPET - 0으로 나누기 방지
    }

    const unsigned int timerConfigOffset = kTimerConfigOffset(timerIndex);
    const unsigned long timerConfig = kReadReg(timerConfigOffset);
    if (!(timerConfig & kTimerConfigPeriodicCapableBit)) {
        return false;  // 이 비교기가 주기 모드를 지원 안 함
    }

    // 항상 GSI2에 고정하는 Legacy Replacement 대신, 이 타이머가 실제로
    // 라우팅 가능한 GSI를 Tn_INT_ROUTE_CAP 비트맵에서 직접 고른다
    // (QU-1CC6BB1D, 설계자 지시, 2026-09-14 - "고정된 Legacy
    // Replacement가 아닌 비트맵을 읽어 원하는 GSI를 골라야 한다").
    const unsigned int gsi = kPickGsiFromRouteCap(timerConfig);
    if (gsi == kInvalidTimerIndex) {
        return false;  // 이 타이머가 라우팅 가능한 GSI가 하나도 없음(비정상)
    }

    // 레벨 트리거로 설정(HPET 스펙 권장 - IOAPIC 임의 GSI 라우팅은
    // 여러 장치가 같은 핀을 공유할 수도 있어 엣지보다 레벨이 안전),
    // active-high로 IOAPIC에 반영한다.
    const bool routed = IoApic::setRedirection(gsi, vector, destApicId, kAcpiPolarityActiveHigh, kAcpiTriggerLevel);
    if (!routed) {
        return false;  // destApicId가 8비트 한도 초과거나 gsi가 이 IOAPIC 범위 밖
    }

    const unsigned long ticksPerInterval = (kFemtosecondsPerSecond / frequencyHz) / periodFemtoseconds;

    Idt::registerHandler(vector, kHpetInterruptHandler);
    gVectorToTimerIndex[vector & 0xFF] = timerIndex;

    // 주기 모드 설정(HPET 스펙 2.3.9.2.1) - TIMER_VAL_SET_CNF를 세운
    // 채로 comparator에 값을 두 번 연속 써야 한다: 첫 번째는 "다음
    // 인터럽트까지 남은 값"(초기값), 두 번째가 실제 주기(period)로
    // 확정된다(하드웨어가 TIMER_VAL_SET_CNF를 그 두 번째 쓰기 직후
    // 자동으로 지운다) - 초기 지연과 주기를 같은 값으로 두면 그냥
    // 같은 값을 두 번 쓰면 된다. 이 두 번 쓰기를 빼먹으면(한 번만
    // 쓰면) 실제 하드웨어/일부 에뮬레이터에서 주기가 안 걸리고 한 번
    // 쏘고 멈추는 것처럼 보일 수 있다 - 관계도에 기록.
    unsigned long config = timerConfig;
    config |= kTimerConfigIntEnableBit | kTimerConfigPeriodicBit | kTimerConfigValSetBit | kTimerConfigTypeLevelBit;
    config &= ~kTimerConfigIntRouteMask;
    config |= (static_cast<unsigned long>(gsi) << kTimerConfigIntRouteShift) & kTimerConfigIntRouteMask;
    kWriteReg(timerConfigOffset, config);
    kWriteReg(kTimerComparatorOffset(timerIndex), ticksPerInterval);
    kWriteReg(kTimerComparatorOffset(timerIndex), ticksPerInterval);

    return true;
}

}  // namespace kernel
