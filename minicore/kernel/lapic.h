#ifndef MINICORE_KERNEL_LAPIC_H
#define MINICORE_KERNEL_LAPIC_H

namespace kernel {

// 이 프로젝트는 처음부터 레거시 PIC(8259)가 아니라 Local APIC를
// 쓴다 - SMP에서는 코어마다 자기 LAPIC이 있어야 타이머/IPI(코어간
// 인터럽트)가 되고, PIC은 애초에 코어를 지정해서 인터럽트를 줄 수
// 없다(설계자 지시 - "SMP 부팅을 고려"). 지금은 BSP(지금 실행 중인
// 코어) 하나만 초기화한다 - AP(나머지 코어) 기동은 ACPI MADT 파싱+
// 16비트 트램폴린이 필요한 별도 마일스톤(아직 없음, DS-D4E5C451
// 참고).
class Lapic {
public:
    // 레거시 PIC을 전부 마스크하고, LAPIC을 MMIO로 매핑한 뒤
    // 소프트웨어로 활성화한다(Spurious Interrupt Vector Register).
    static void init();

    static unsigned int id();

    // 인터럽트 핸들러가 처리를 마치면 반드시 호출해야 한다 - 안 하면
    // 그 이하 우선순위 인터럽트가 더는 안 들어온다.
    static void sendEoi();

    // Timer.cpp가 LVT Timer/Divide/Initial Count를 직접 쓸 때 쓴다.
    static void writeRegister(unsigned int offset, unsigned int value);
    static unsigned int readRegister(unsigned int offset);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_LAPIC_H
