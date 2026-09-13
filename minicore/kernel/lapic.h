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
//
// x2APIC(CPUID로 지원 여부 감지)이 있으면 그쪽을 쓰고, 없으면 xAPIC
// (MMIO)으로 폴백한다(PL-D65F49CC, 설계자 지시 - "X2APIC도 고려").
// 공개 API(id/isReady/sendEoi/readRegister/writeRegister)는 두 모드
// 모두 동일하게 쓸 수 있다 - 내부에서 모드에 따라 MSR 접근(x2APIC)과
// MMIO 접근(xAPIC)으로 갈라진다. 단, ICR(0x300/0x310 vs 0x830)처럼
// 레지스터 구성 자체가 다른 부분은 아직 이 클래스에 없다 - SMP AP
// 기동 계획(PL-65C20380)에서 IPI를 보낼 때 이 차이를 반드시 알아야
// 한다.
class Lapic {
public:
    // 레거시 PIC을 전부 마스크하고, CPUID로 x2APIC 지원 여부를 확인해
    // 있으면 x2APIC 모드로, 없으면 LAPIC을 MMIO로 매핑해 xAPIC 모드로
    // 전환한 뒤 소프트웨어로 활성화한다(Spurious Interrupt Vector
    // Register).
    static void init();

    static unsigned int id();

    // init()이 LAPIC 초기화(x2APIC MSR 전환 또는 xAPIC MMIO 매핑)를
    // 끝냈는지 - PageFrameAllocator가 "지금 코어의 NUMA 노드"를
    // 물어보려고 id()를 부르기 전에 반드시 이걸로 먼저 확인해야 한다.
    // xAPIC 경로에서는 init() 자신도 (매핑용 페이지가 필요하면)
    // PageFrameAllocator::allocPage()를 부르는데, 그 시점엔 아직 LAPIC이
    // 안 잡혀 있으니 id()를 부르면 안 된다(닭-달걀 문제, 2026-09-14
    // 실측으로 발견). x2APIC 경로는 MMIO 매핑 자체가 없어 이 문제가
    // 애초에 생기지 않지만, 플래그는 두 경로 공통으로 관리한다.
    static bool isReady();

    // 진단/로그용 - init()이 x2APIC과 xAPIC 중 어느 쪽으로 붙었는지.
    static bool usesX2Apic();

    // 인터럽트 핸들러가 처리를 마치면 반드시 호출해야 한다 - 안 하면
    // 그 이하 우선순위 인터럽트가 더는 안 들어온다.
    static void sendEoi();

    // Timer.cpp가 LVT Timer/Divide/Initial Count를 직접 쓸 때 쓴다.
    static void writeRegister(unsigned int offset, unsigned int value);
    static unsigned int readRegister(unsigned int offset);

    // ICR(Interrupt Command Register) 전송 - SMP AP 기동(PL-65C20380)의
    // INIT-SIPI-SIPI 시퀀스 전용. 다른 레지스터와 달리 xAPIC(ICR_LOW
    // 0x300+ICR_HIGH 0x310, 두 개의 32비트 레지스터)과 x2APIC(MSR
    // 0x830 하나, 64비트 통합)이 근본적으로 다른 유일한 레지스터라
    // readRegister/writeRegister로 일반화할 수 없다 - 그래서 전용
    // 메서드로 따로 뒀다(PL-D65F49CC 설계 당시부터 예견된 차이).
    // assert=true면 INIT 어서트, false면 디어서트(레벨 비트만 다름).
    static void sendInitIpi(unsigned int destApicId, bool assert);
    // startupVector: SIPI가 가리키는 물리주소를 4096으로 나눈 값
    // (예: 0x8000 -> 0x08) - AP가 그 페이지의 오프셋 0부터 16비트
    // 실모드로 시작한다.
    static void sendStartupIpi(unsigned int destApicId, unsigned int startupVector);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_LAPIC_H
