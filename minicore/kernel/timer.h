#ifndef MINICORE_KERNEL_TIMER_H
#define MINICORE_KERNEL_TIMER_H

namespace kernel {

constexpr unsigned int kTimerVector = 32;  // 첫 하드웨어 인터럽트 벡터(CPU 예외 0-31 다음)

// 스케줄러 틱의 시간원을 초기화한다(DS-D4E5C451, QU-8E14D3D9) - HPET가
// 있고 Timer0이 주기 모드를 지원하면 그쪽을 기본으로 쓰고(hpet.cpp),
// 없거나 지원 안 하면 LAPIC 타이머를 PIT(채널 2)로 보정해서 100Hz
// (10ms) 주기 인터럽트로 프로그래밍하는 기존 경로로 폴백한다. 어느
// 쪽이든 결과적으로 onTick()을 호출하므로 tickCount()의 의미는
// 시간원과 무관하게 동일하다. Acpi::init()/IoApic::init()/Lapic::init()
// 이후에 호출해야 한다.
class Timer {
public:
    static void init();
    static unsigned long tickCount();

    // 진단/로그용 - 실제로 HPET을 시간원으로 쓰는지(false면 LAPIC/PIT
    // 폴백 경로).
    static bool usesHpet();

    // idt.cpp의 타이머 ISR(vector kTimerVector)이 호출한다.
    static void onTick();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_TIMER_H
