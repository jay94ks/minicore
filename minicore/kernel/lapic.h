#ifndef MINICORE_KERNEL_LAPIC_H
#define MINICORE_KERNEL_LAPIC_H

#include "libkenv/types.h"

namespace kernel {

// LAPIC(xAPIC MMIO 오프셋 기준 - x2APIC은 Lapic::readRegister/
// writeRegister가 내부에서 MSR 0x800+(offset>>4)로 자동 변환) 표준
// 레지스터 오프셋 전부(Intel SDM Vol.3 11.4.1) - QU-B569F367, 설계자
// 지시, 2026-09-14: "LAPIC에 존재하는 모든 레지스터를 구현해놓고
// 호환성 옵션들을 추가해야 한다." readRegister/writeRegister는
// 이전부터 임의 오프셋을 받을 수 있었지만, 이 이름들을 통해 어떤
// 레지스터가 있는지 명시적으로 드러낸다.
constexpr uint32_t kLapicRegId = 0x020;
constexpr uint32_t kLapicRegVersion = 0x030;
constexpr uint32_t kLapicRegTaskPriority = 0x080;         // TPR
constexpr uint32_t kLapicRegArbitrationPriority = 0x090;  // APR, 읽기전용
constexpr uint32_t kLapicRegProcessorPriority = 0x0A0;    // PPR, 읽기전용
constexpr uint32_t kLapicRegEoi = 0x0B0;
constexpr uint32_t kLapicRegRemoteRead = 0x0C0;  // RRD, deprecated - 구형 하드웨어 호환용
constexpr uint32_t kLapicRegLogicalDestination = 0x0D0;    // LDR - xAPIC 전용
constexpr uint32_t kLapicRegDestinationFormat = 0x0E0;     // DFR - xAPIC 전용
constexpr uint32_t kLapicRegSpuriousVector = 0x0F0;        // SVR
constexpr uint32_t kLapicRegInService0 = 0x100;    // ISR0-7 (0x100,0x110,...,0x170), 읽기전용
constexpr uint32_t kLapicRegTriggerMode0 = 0x180;  // TMR0-7, 읽기전용
constexpr uint32_t kLapicRegInterruptRequest0 = 0x200;  // IRR0-7, 읽기전용
constexpr uint32_t kLapicRegErrorStatus = 0x280;   // ESR - 읽기 전 0을 한 번 써야 최신값 반영(스펙 quirk)
constexpr uint32_t kLapicRegIcrLow = 0x300;
constexpr uint32_t kLapicRegIcrHigh = 0x310;
constexpr uint32_t kLapicRegLvtTimer = 0x320;
constexpr uint32_t kLapicRegLvtThermal = 0x330;
constexpr uint32_t kLapicRegLvtPerfCounter = 0x340;
constexpr uint32_t kLapicRegLvtLint0 = 0x350;
constexpr uint32_t kLapicRegLvtLint1 = 0x360;
constexpr uint32_t kLapicRegLvtError = 0x370;
constexpr uint32_t kLapicRegInitialCount = 0x380;
constexpr uint32_t kLapicRegCurrentCount = 0x390;
constexpr uint32_t kLapicRegDivideConfig = 0x3E0;

constexpr uint32_t kLapicLvtMaskedBit = 1U << 16;

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

    static uint32_t id();

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

    // 커널 커맨드라인 `--disable-x2apic`(QU-6ABACEAD, 설계자 지시,
    // 2026-09-14 - "커널 옵션으로 --disable-x2apic를 받으면
    // 비활성화되도록 구현하라. 이 기능이 있어야 호환되지 않는
    // 하드웨어에서도 사용자의 수동 설정 등을 통하여 정상 동작을
    // 보장할 수 있다")로 켠다 - true면 CPUID가 x2APIC을 지원해도
    // init()이 강제로 xAPIC 경로를 쓴다. init() 호출 **전에** 설정해야
    // 의미가 있다.
    static void setX2ApicDisabled(bool disabled);

    // TPR(Task Priority Register) - priority보다 낮은 우선순위
    // 클래스의 인터럽트는 이 코어에 전달되지 않는다. 0=전부 수신
    // (기본값, init()이 설정).
    static void setTaskPriority(uint32_t priority);
    static uint32_t taskPriority();
    // PPR(Processor Priority Register) - 읽기전용, 실제 유효
    // 우선순위(TPR과 최고 ISR 비트 중 큰 값).
    static uint32_t processorPriority();

    // LDR/DFR - xAPIC 전용(x2APIC은 목적지가 항상 물리 ID라 이
    // 레지스터들이 없음 - 호출해도 무해하게 무시됨). 지금은 이
    // 프로젝트가 물리 목적지 모드만 쓰므로(IOAPIC REDTBL/ICR 전부
    // physical mode) 실제로 켤 일은 없지만, 논리 목적지 모드가
    // 필요해질 미래를 대비해 노출해 둔다("호환성 옵션" - QU-B569F367).
    static void setLogicalDestination(uint32_t logicalId);
    static void setDestinationFormat(uint32_t format);

    // 인터럽트 핸들러가 처리를 마치면 반드시 호출해야 한다 - 안 하면
    // 그 이하 우선순위 인터럽트가 더는 안 들어온다.
    static void sendEoi();

    // Timer.cpp가 LVT Timer/Divide/Initial Count를 직접 쓸 때 쓴다.
    static void writeRegister(uint32_t offset, uint32_t value);
    static uint32_t readRegister(uint32_t offset);

    // ICR(Interrupt Command Register) 전송 - SMP AP 기동(PL-65C20380)의
    // INIT-SIPI-SIPI 시퀀스 전용. 다른 레지스터와 달리 xAPIC(ICR_LOW
    // 0x300+ICR_HIGH 0x310, 두 개의 32비트 레지스터)과 x2APIC(MSR
    // 0x830 하나, 64비트 통합)이 근본적으로 다른 유일한 레지스터라
    // readRegister/writeRegister로 일반화할 수 없다 - 그래서 전용
    // 메서드로 따로 뒀다(PL-D65F49CC 설계 당시부터 예견된 차이).
    // assert=true면 INIT 어서트, false면 디어서트(레벨 비트만 다름).
    static void sendInitIpi(uint32_t destApicId, bool assert);
    // startupVector: SIPI가 가리키는 물리주소를 4096으로 나눈 값
    // (예: 0x8000 -> 0x08) - AP가 그 페이지의 오프셋 0부터 16비트
    // 실모드로 시작한다.
    static void sendStartupIpi(uint32_t destApicId, uint32_t startupVector);

    // 이 코어의 LAPIC 자체 주기 타이머를 PIT 채널2로 보정해 hz 주기로
    // 프로그래밍하고 vector로 인터럽트를 걸어 켠다 - LAPIC 타이머는
    // 코어마다 독립된 하드웨어라 BSP/AP가 각자 호출해도 서로 간섭하지
    // 않는다(QU-CFAA5B3D, 설계자 지시, 2026-09-14 - "AP 개별 LAPIC
    // 타이머도 이번 테스트 범위에 포함시켜"). 원래 Timer::init()의
    // LAPIC+PIT 폴백 경로에 있던 보정 로직을 여기로 옮겨 AP도 재사용할
    // 수 있게 했다. **주의**: 보정에 쓰는 PIT 채널2는 전역 자원이라
    // 여러 코어가 동시에 호출하면 안 된다 - 지금은 AP 기동이 순차적
    // (한 코어씩 완전히 켠 뒤 다음 코어로)이라 안전하지만, 병렬 AP
    // 기동을 도입하면 재검토 필요.
    static void startPeriodicTimer(uint32_t vector, uint32_t hz);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_LAPIC_H
