#ifndef MINICORE_KERNEL_HPET_H
#define MINICORE_KERNEL_HPET_H

namespace kernel {

constexpr unsigned int kHpetVector = 0x22;  // isr.S 동적 벡터(33-254) 대역 중 하나

// HPET가 있으면 스케줄러 기본 시간원으로 쓰고, LAPIC 타이머는
// fallback으로 유지한다(DS-D4E5C451, QU-8E14D3D9 확정). Timer0을
// Legacy Replacement Route로 설정해 GSI2(IOAPIC)를 거쳐 100Hz 주기
// 인터럽트를 낸다 - PIT/LAPIC 타이머 경로(timer.cpp)와 똑같이
// Timer::onTick()을 불러서, 스케줄러 입장에서는 어느 하드웨어가
// 실제 시간원인지 구분할 필요가 없다.
class Hpet {
public:
    // Acpi::init() 이후, IoApic::init()/Lapic::init() 이후에 호출해야
    // 한다(리다이렉션 설정에 둘 다 필요). Timer0이 주기 모드를
    // 지원하지 않는 등 초기화에 실패하면 false를 반환한다 - 호출부
    // (Timer::init())가 이 경우 LAPIC/PIT 폴백 경로로 넘어가야 한다.
    static bool init();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_HPET_H
