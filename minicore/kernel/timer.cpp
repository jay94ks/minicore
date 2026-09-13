#include "timer.h"

#include "x86_64/io_port.h"
#include "acpi.h"
#include "hpet.h"
#include "lapic.h"

namespace {

constexpr unsigned int kLapicDivideConfig = 0x3E0;
constexpr unsigned int kLapicLvtTimer = 0x320;
constexpr unsigned int kLapicInitialCount = 0x380;
constexpr unsigned int kLapicCurrentCount = 0x390;

constexpr unsigned int kDivideBy16 = 0x3;
constexpr unsigned int kLvtMaskedBit = 1U << 16;
constexpr unsigned int kLvtPeriodicBit = 1U << 17;

constexpr unsigned short kPitChannel2Data = 0x42;
constexpr unsigned short kPitCommand = 0x43;
constexpr unsigned short kPitGateControl = 0x61;  // NMI/스피커 제어 포트
constexpr unsigned int kPitFrequencyHz = 1193182;
constexpr unsigned int kCalibrationMs = 10;

unsigned long gTickCount = 0;
bool gUsesHpet = false;

// PIT 채널2를 kCalibrationMs만큼 원샷으로 돌리는 동안, 이미 최댓값
// (0xFFFFFFFF)에서 카운트다운 중인 LAPIC 타이머가 얼마나 줄었는지
// 재서 "그 시간 동안의 LAPIC 틱 수"를 구한다 - 그 값이 그대로
// 원하는 주기(같은 kCalibrationMs)의 initial count가 된다.
unsigned int kCalibrateLapicTicksPerWindow() {
    const unsigned int pitCount = kPitFrequencyHz / (1000 / kCalibrationMs);

    kernel::arch::kOutB(kPitGateControl, kernel::arch::kInB(kPitGateControl) & 0xFC);  // 게이트/스피커 끄기
    kernel::arch::kOutB(kPitCommand, 0xB0);                                     // 채널2, lobyte/hibyte, 모드0
    kernel::arch::kOutB(kPitChannel2Data, static_cast<unsigned char>(pitCount & 0xFF));
    kernel::arch::kOutB(kPitChannel2Data, static_cast<unsigned char>((pitCount >> 8) & 0xFF));

    kernel::Lapic::writeRegister(kLapicDivideConfig, kDivideBy16);
    kernel::Lapic::writeRegister(kLapicLvtTimer, kLvtMaskedBit);
    kernel::Lapic::writeRegister(kLapicInitialCount, 0xFFFFFFFF);

    kernel::arch::kOutB(kPitGateControl, (kernel::arch::kInB(kPitGateControl) & 0xFC) | 0x01);  // 게이트 켜서 카운트다운 시작

    while (!(kernel::arch::kInB(kPitGateControl) & 0x20)) {
        // OUT2(비트5)가 설 때까지 대기 - PIT 원샷 카운트 만료 신호
    }

    kernel::arch::kOutB(kPitGateControl, kernel::arch::kInB(kPitGateControl) & 0xFC);  // 게이트 끄기

    const unsigned int current = kernel::Lapic::readRegister(kLapicCurrentCount);
    return 0xFFFFFFFFU - current;
}

}  // namespace

namespace kernel {

void Timer::init() {
    if (Acpi::hasHpet() && Hpet::init()) {
        gUsesHpet = true;
        return;  // HPET가 스케줄러 틱을 담당 - 아래 PIT 보정/LAPIC 주기 설정은 불필요
    }

    const unsigned int ticksPerWindow = kCalibrateLapicTicksPerWindow();

    Lapic::writeRegister(kLapicDivideConfig, kDivideBy16);
    Lapic::writeRegister(kLapicLvtTimer, kTimerVector | kLvtPeriodicBit);
    Lapic::writeRegister(kLapicInitialCount, ticksPerWindow);
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

}  // namespace kernel
