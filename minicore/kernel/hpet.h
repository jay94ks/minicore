#ifndef MINICORE_KERNEL_HPET_H
#define MINICORE_KERNEL_HPET_H

#include "libkenv/types.h"

namespace kernel {

constexpr uint32_t kHpetVector = 0x22;  // isr.S 동적 벡터(33-254) 대역 중 하나 - Timer0(기본 시간원) 전용

// HPET가 있으면 스케줄러 기본 시간원으로 쓰고, LAPIC 타이머는
// fallback으로 유지한다(DS-D4E5C451, QU-8E14D3D9 확정). Timer0을
// 100Hz 주기 인터럽트로 설정해 PIT/LAPIC 타이머 경로(timer.cpp)와
// 똑같이 Timer::onTick()을 불러서, 스케줄러 입장에서는 어느 하드웨어가
// 실제 시간원인지 구분할 필요가 없다.
//
// **멀티 비교기 지원(QU-2247A01E, 설계자 지시, 2026-09-14 - "HPET을
// 여러 개 비교기로 확장하여 멀티 타이머를 구성해야 한다. 거의
// 확정적으로 코어별 독립 틱이 필요해질 것이다")**: Timer0(기본)
// 이외의 비교기를 원하는 주파수/벡터/목적지로 독립 설정할 수 있는
// enableTimer()를 공개 API로 뒀다 - 아직 SMP 스케줄러가 없어 실제
// "코어별 독립 틱"을 거는 호출부는 없지만, 그 시점에 이 API를 코어
// 수만큼 반복 호출하면 된다(HPET가 제공하는 비교기 개수는
// timerCount()로 확인 - 스펙상 최소 3개는 보장되지만 실제로는
// 그보다 적거나 많을 수 있다).
class Hpet {
public:
    // Acpi::init() 이후, IoApic::init()/Lapic::init() 이후에 호출해야
    // 한다(리다이렉션 설정에 둘 다 필요). Timer0이 주기 모드를
    // 지원하지 않는 등 초기화에 실패하면 false를 반환한다 - 호출부
    // (Timer::init())가 이 경우 LAPIC/PIT 폴백 경로로 넘어가야 한다.
    static bool init();

    // 이 HPET가 제공하는 비교기(타이머) 개수 - General Capabilities의
    // NUM_TIM_CAP 필드+1. init() 이후에만 유효하다(그 전엔 0).
    static uint32_t timerCount();

    // timerIndex(0 ~ timerCount()-1)번 비교기를 frequencyHz 주기
    // 인터럽트로 설정하고, IOAPIC의 Tn_INT_ROUTE_CAP 비트맵에서 실제로
    // 라우팅 가능한 GSI를 골라 vector/destApicId로 연결한다 - 인터럽트가
    // 오면 Timer::onTick()을 호출한다(시간원이 여럿이어도 tickCount()
    // 의미는 동일하게 유지). 이미 켜져 있던 비교기를 다시 부르면 새
    // 설정으로 덮어쓴다. 실패(주기 모드 미지원/라우팅 가능한 GSI 없음/
    // destApicId가 IOAPIC 8비트 한도 초과 등)하면 false.
    static bool enableTimer(uint32_t timerIndex, uint32_t frequencyHz, uint32_t vector,
                             uint32_t destApicId);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_HPET_H
