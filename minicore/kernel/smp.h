#ifndef MINICORE_KERNEL_SMP_H
#define MINICORE_KERNEL_SMP_H

#include "libkenv/types.h"

namespace kernel {

// SMP AP(BSP 이외 코어) 기동 (PL-65C20380). ACPI MADT로 확보한 코어
// 목록(Acpi::cpuCount()/cpuApicId())을 훑어 BSP 자신이 아닌 코어마다
// INIT-SIPI-SIPI를 순차적으로 보낸다(한 번에 AP 하나씩 - 트램폴린의
// BSP-AP 핸드오프 스크래치 슬롯을 공유하기 때문에 동시 기동은 하지
// 않는다). 각 AP가 "기동됨" 신호(원자 카운터)를 보낼 때까지 타임아웃
// 대기한 뒤 다음 AP로 넘어간다 - 응답이 없으면 SIPI를 한 번 더
// 보내고(구식 CPU 대응) 마지막으로 한 번 더 기다린다.
//
// Acpi::init()/PageFrameAllocator::init()/Lapic::init()/IoApic::init()/
// Timer::init() 전부 끝나고, 인터럽트가 켜진(sti) 뒤에 호출해야 한다
// (Timer 틱 기반 타임아웃 대기가 인터럽트에 의존함).
//
// 이 계획 자체의 검증 범위는 "AP가 실제로 깨어나 시리얼에 자기 APIC
// ID를 한 번 찍고 idle(hlt 루프)에 들어간다"까지다 - 스케줄러가 없어
// AP가 실제로 할 일을 받는 건 범위 밖(QA-26450C3E 참고).
class Smp {
public:
    static void startApCores();

    // 진단/로그용 - 실제로 기동에 성공한 AP 수(BSP 제외).
    static uint32_t startedCount();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SMP_H
