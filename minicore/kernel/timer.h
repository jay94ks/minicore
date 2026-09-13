#ifndef MINICORE_KERNEL_TIMER_H
#define MINICORE_KERNEL_TIMER_H

namespace kernel {

constexpr unsigned int kTimerVector = 32;  // 첫 하드웨어 인터럽트 벡터(CPU 예외 0-31 다음)

// LAPIC 타이머를 PIT(채널 2)로 보정해서 100Hz(10ms) 주기 인터럽트로
// 프로그래밍한다. Lapic::init() 이후에 호출해야 한다.
class Timer {
public:
    static void init();
    static unsigned long tickCount();

    // idt.cpp의 타이머 ISR(vector kTimerVector)이 호출한다.
    static void onTick();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_TIMER_H
