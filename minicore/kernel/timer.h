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

    // 레거시 PIT(채널0) IRQ0을 IOAPIC으로 라우팅해 세 번째(레거시)
    // 폴백 시간원으로 켤 수 있게 한다(QU-FC9D80FF, 설계자 지시,
    // 2026-09-14 - "legacy fallback을 지원하기 위해서는 구현되어
    // 있고, 커널 내에서 끄거나 킬 수 있도록 동적 구성되어야 한다").
    // 기본은 꺼져 있다 - HPET(기본)/LAPIC(폴백)로 이미 충분하고, 이건
    // IOAPIC/ACPI 라우팅 자체가 고장난 호환성 문제를 우회하기 위한
    // 마지막 안전망이다. 켜져 있는 동안에도 onTick()을 호출해
    // tickCount()에 합산된다 - 여러 시간원을 동시에 켜면 틱이 여러
    // 번 카운트될 수 있다는 뜻이니 호출부가 알아서 판단해야 한다.
    static bool enableLegacyPitIrq();
    static void disableLegacyPitIrq();
    static bool isLegacyPitIrqEnabled();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_TIMER_H
