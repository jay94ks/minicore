#include "lapic.h"

#include "x86_64/io_port.h"
#include "libkenv/types.h"
#include "paging.h"

namespace {

constexpr kernel::uint64_t kIa32ApicBaseMsr = 0x1B;
constexpr kernel::uint64_t kApicBaseEnableBit = 1UL << 11;
constexpr kernel::uint64_t kApicBaseExtdBit = 1UL << 10;  // x2APIC 모드 전환 비트
constexpr kernel::uint64_t kApicBaseAddrMask = 0x000FFFFFFFFFF000UL;

constexpr kernel::uint32_t kSpuriousSoftwareEnableBit = 1U << 8;

constexpr kernel::uint32_t kX2ApicIcrMsr = 0x830;  // x2APIC은 ICR이 64비트 MSR 하나로 통합됨(xAPIC의 0x300+0x310과 다름)

constexpr kernel::uint32_t kLapicDivideBy16 = 0x3;
constexpr kernel::uint32_t kLapicLvtPeriodicBit = 1U << 17;

constexpr kernel::uint16_t kPitChannel2Data = 0x42;
constexpr kernel::uint16_t kPitCommand = 0x43;
constexpr kernel::uint16_t kPitGateControl = 0x61;  // NMI/스피커 제어 포트
constexpr kernel::uint32_t kPitFrequencyHz = 1193182;
constexpr kernel::uint32_t kCalibrationMs = 10;

constexpr kernel::uint32_t kIcrDeliveryModeInit = 5U << 8;
constexpr kernel::uint32_t kIcrDeliveryModeStartup = 6U << 8;
constexpr kernel::uint32_t kIcrLevelAssert = 1U << 14;
constexpr kernel::uint32_t kIcrTriggerModeLevel = 1U << 15;
constexpr kernel::uint32_t kIcrDeliveryStatusBit = 1U << 12;  // x2APIC엔 없음(전송이 항상 동기적으로 완료됨)
constexpr kernel::uint32_t kXApicIcrHighDestShift = 24;

// LAPIC MMIO는 direct map(WB 캐시)에 그대로 얹으면 안 된다 - 전용
// 가상주소에 캐시 비활성으로 따로 매핑한다. (x2APIC 모드에서는 MMIO
// 매핑 자체를 안 쓴다 - 전부 MSR 접근이라 필요 없음.)
constexpr kernel::uint64_t kLapicVirtBase = 0xFFFF901000000000UL;

// x2APIC 레지스터는 MSR 0x800 + (MMIO 오프셋 >> 4)로 접근한다(Intel
// SDM Vol.3 10.12.1) - ID 레지스터(MMIO 0x020)는 MSR 0x802.
constexpr kernel::uint32_t kX2ApicMsrBase = 0x800;
constexpr kernel::uint32_t kX2ApicIdMsr = 0x802;

kernel::uint64_t gLapicVirtAddr = 0;
// xAPIC은 gLapicVirtAddr(!=0)로 준비 여부를 판단했지만, x2APIC은 MMIO
// 매핑이 아예 없어 그 값이 0으로 남는다 - 그래서 준비 플래그를 따로
// 둔다(PageFrameAllocator가 kCurrentNumaNode()에서 물어보는 대상).
bool gLapicReady = false;
// CPUID.01H:ECX 비트21로 감지한 x2APIC 지원 여부 + 실제 그 모드로
// 전환했는지 - init() 시작 시 한 번만 결정하고 이후 고정한다(부팅
// 중 xAPIC<->x2APIC을 오가는 경로는 지원하지 않음, DC 없음: PL-D65F49CC
// 설계 범위 내 결정).
bool gUseX2Apic = false;
// --disable-x2apic 커널 커맨드라인 옵션(QU-6ABACEAD) - true면 CPUID가
// x2APIC을 지원해도 init()이 강제로 xAPIC을 쓴다.
bool gX2ApicDisabledByOption = false;

// CPUID.01H:ECX 비트21 - x2APIC 지원 여부. ebx는 그냥 버리지만 cpuid는
// eax/ebx/ecx/edx를 전부 건드리므로 전부 출력 제약에 넣어야 한다.
bool kCpuidHasX2Apic() {
    kernel::uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    constexpr kernel::uint32_t kX2ApicCpuidBit = 1U << 21;
    return (ecx & kX2ApicCpuidBit) != 0;
}

// PIC을 CPU 예외 벡터(0-31)와 절대 안 겹치는 곳으로 옮겨 둔다(0xE0
// 마스터/0xE8 슬레이브 - kTimerVector=32, spurious=0xFF와도 안
// 겹치게 골랐다). 완전히 마스크해서 정상적으로는 절대 안 쓰지만,
// 리매핑까지 해 두는 이유는 마스크돼 있어도 드물게 spurious PIC
// 인터럽트(레거시 IRQ7/15)가 새어 나올 수 있기 때문이다 - 리매핑을
// 안 해 두면 그게 기본 벡터(0x08=더블폴트 등)로 들어와 진짜 CPU
// 예외로 오진될 수 있다(설계자 지적, 2026-09-14).
constexpr kernel::uint8_t kPicMasterVectorBase = 0xE0;
constexpr kernel::uint8_t kPicSlaveVectorBase = 0xE8;

void kDisableLegacyPic() {
    // 이 프로젝트는 PIC이 아니라 LAPIC/IOAPIC을 쓴다(설계자 지시 -
    // SMP 고려). ICW1~4로 표준 리매핑 시퀀스를 거친 뒤 전부 마스크한다.
    kernel::arch::kOutB(0x20, 0x11);  // ICW1: 마스터, cascade, ICW4 필요
    kernel::arch::kOutB(0xA0, 0x11);  // ICW1: 슬레이브
    kernel::arch::kOutB(0x21, kPicMasterVectorBase);
    kernel::arch::kOutB(0xA1, kPicSlaveVectorBase);
    kernel::arch::kOutB(0x21, 0x04);  // ICW3: 마스터 - IRQ2에 슬레이브 연결됨
    kernel::arch::kOutB(0xA1, 0x02);  // ICW3: 슬레이브 - 자신의 cascade identity
    kernel::arch::kOutB(0x21, 0x01);  // ICW4: 8086 모드
    kernel::arch::kOutB(0xA1, 0x01);  // ICW4: 8086 모드

    kernel::arch::kOutB(0x21, 0xFF);  // 마스터 전 라인 마스크
    kernel::arch::kOutB(0xA1, 0xFF);  // 슬레이브 전 라인 마스크
}

// PIT 채널2를 kCalibrationMs만큼 원샷으로 돌리는 동안, 이미 최댓값
// (0xFFFFFFFF)에서 카운트다운 중인 LAPIC 타이머가 얼마나 줄었는지
// 재서 "그 시간 동안의 LAPIC 틱 수"를 구한다 - 그 값이 그대로 원하는
// 주기(같은 kCalibrationMs)의 initial count가 된다.
kernel::uint32_t kCalibrateLapicTicksPerWindow() {
    const kernel::uint32_t pitCount = kPitFrequencyHz / (1000 / kCalibrationMs);

    kernel::arch::kOutB(kPitGateControl, kernel::arch::kInB(kPitGateControl) & 0xFC);  // 게이트/스피커 끄기
    kernel::arch::kOutB(kPitCommand, 0xB0);                                     // 채널2, lobyte/hibyte, 모드0
    kernel::arch::kOutB(kPitChannel2Data, static_cast<kernel::uint8_t>(pitCount & 0xFF));
    kernel::arch::kOutB(kPitChannel2Data, static_cast<kernel::uint8_t>((pitCount >> 8) & 0xFF));

    kernel::Lapic::writeRegister(kernel::kLapicRegDivideConfig, kLapicDivideBy16);
    kernel::Lapic::writeRegister(kernel::kLapicRegLvtTimer, kernel::kLapicLvtMaskedBit);
    kernel::Lapic::writeRegister(kernel::kLapicRegInitialCount, 0xFFFFFFFF);

    kernel::arch::kOutB(kPitGateControl, (kernel::arch::kInB(kPitGateControl) & 0xFC) | 0x01);  // 게이트 켜서 카운트다운 시작

    while (!(kernel::arch::kInB(kPitGateControl) & 0x20)) {
        // OUT2(비트5)가 설 때까지 대기 - PIT 원샷 카운트 만료 신호
    }

    kernel::arch::kOutB(kPitGateControl, kernel::arch::kInB(kPitGateControl) & 0xFC);  // 게이트 끄기

    const kernel::uint32_t current = kernel::Lapic::readRegister(kernel::kLapicRegCurrentCount);
    return 0xFFFFFFFFU - current;
}

kernel::uint64_t kReadMsr(kernel::uint64_t msr) {
    kernel::uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<kernel::uint64_t>(high) << 32) | low;
}

void kWriteMsr(kernel::uint64_t msr, kernel::uint64_t value) {
    const auto low = static_cast<kernel::uint32_t>(value & 0xFFFFFFFF);
    const auto high = static_cast<kernel::uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

}  // namespace

namespace kernel {

void Lapic::init() {
    kDisableLegacyPic();

    gUseX2Apic = !gX2ApicDisabledByOption && kCpuidHasX2Apic();
    kernel::uint64_t apicBaseMsr = kReadMsr(kIa32ApicBaseMsr);

    if (gUseX2Apic) {
        // x2APIC은 MSR 하나로 활성화+모드전환이 끝난다 - MMIO 매핑이
        // 없으므로 Paging::mapPage/PageFrameAllocator를 안 거친다(그래서
        // xAPIC 경로에 있던 Lapic<->PageFrameAllocator 닭-달걀 문제가
        // 이 경로에서는 애초에 발생하지 않는다).
        apicBaseMsr |= kApicBaseEnableBit | kApicBaseExtdBit;
        kWriteMsr(kIa32ApicBaseMsr, apicBaseMsr);
    } else {
        if (!(apicBaseMsr & kApicBaseEnableBit)) {
            apicBaseMsr |= kApicBaseEnableBit;
            kWriteMsr(kIa32ApicBaseMsr, apicBaseMsr);
        }
        const kernel::uint64_t lapicPhysAddr = apicBaseMsr & kApicBaseAddrMask;
        Paging::mapPage(kLapicVirtBase, lapicPhysAddr, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
        gLapicVirtAddr = kLapicVirtBase;
    }

    gLapicReady = true;

    // 소프트웨어 활성화 + spurious 인터럽트 벡터(0xFF, 관례상 흔히
    // 쓰는 값 - 하위 4비트가 전부 1이라 우선순위 그룹 규칙과도 맞음).
    writeRegister(kLapicRegSpuriousVector, kSpuriousSoftwareEnableBit | 0xFF);

    // "호환성 옵션"(QU-B569F367) - 아직 안 쓰는 LVT 엔트리(Thermal/
    // PerfCounter/LINT0/LINT1/Error)를 명시적으로 마스크해 둔다. 이
    // 레지스터들은 리셋 직후 값이 정의돼 있지 않은 하드웨어도 있어서
    // (실기에서는 흔히 마스크 상태로 리셋되지만 보장은 아님), 안
    // 마스크해 두면 아직 등록도 안 한 벡터로 예상 못한 인터럽트가
    // 들어올 수 있다 - Linux 등 실제 OS도 LAPIC 초기화 시 이렇게
    // 방어적으로 마스크한다. TPR도 0(모든 우선순위 수신)으로
    // 명시적으로 맞춘다 - 리셋값이 이미 0이지만, x2APIC 강제
    // 비활성화 같은 재초기화 경로를 감안해 항상 확정해 둔다.
    writeRegister(kLapicRegLvtThermal, kLapicLvtMaskedBit);
    writeRegister(kLapicRegLvtPerfCounter, kLapicLvtMaskedBit);
    writeRegister(kLapicRegLvtLint0, kLapicLvtMaskedBit);
    writeRegister(kLapicRegLvtLint1, kLapicLvtMaskedBit);
    writeRegister(kLapicRegLvtError, kLapicLvtMaskedBit);
    setTaskPriority(0);
}

kernel::uint32_t Lapic::id() {
    if (gUseX2Apic) {
        // x2APIC ID 레지스터는 MSR 하위 32비트에 ID가 그대로 들어있다
        // (xAPIC MMIO처럼 상위 8비트로 시프트되어 있지 않음 - 8비트
        // 제한도 없어져 32비트 전체를 ID로 쓸 수 있다).
        return static_cast<kernel::uint32_t>(kReadMsr(kX2ApicIdMsr));
    }
    return readRegister(kLapicRegId) >> 24;
}

bool Lapic::isReady() {
    return gLapicReady;
}

bool Lapic::usesX2Apic() {
    return gUseX2Apic;
}

void Lapic::setX2ApicDisabled(bool disabled) {
    gX2ApicDisabledByOption = disabled;
}

void Lapic::sendEoi() {
    writeRegister(kLapicRegEoi, 0);
}

void Lapic::setTaskPriority(kernel::uint32_t priority) {
    writeRegister(kLapicRegTaskPriority, priority & 0xFF);
}

kernel::uint32_t Lapic::taskPriority() {
    return readRegister(kLapicRegTaskPriority) & 0xFF;
}

kernel::uint32_t Lapic::processorPriority() {
    return readRegister(kLapicRegProcessorPriority) & 0xFF;
}

void Lapic::setLogicalDestination(kernel::uint32_t logicalId) {
    if (gUseX2Apic) {
        return;  // x2APIC엔 LDR이 없음(항상 물리 목적지) - 무해하게 무시
    }
    writeRegister(kLapicRegLogicalDestination, logicalId << 24);
}

void Lapic::setDestinationFormat(kernel::uint32_t format) {
    if (gUseX2Apic) {
        return;  // x2APIC엔 DFR이 없음 - 무해하게 무시
    }
    writeRegister(kLapicRegDestinationFormat, format);
}

void Lapic::writeRegister(kernel::uint32_t offset, kernel::uint32_t value) {
    if (gUseX2Apic) {
        kWriteMsr(kX2ApicMsrBase + (offset >> 4), value);
        return;
    }
    *reinterpret_cast<volatile kernel::uint32_t*>(gLapicVirtAddr + offset) = value;
}

kernel::uint32_t Lapic::readRegister(kernel::uint32_t offset) {
    if (gUseX2Apic) {
        return static_cast<kernel::uint32_t>(kReadMsr(kX2ApicMsrBase + (offset >> 4)));
    }
    return *reinterpret_cast<volatile kernel::uint32_t*>(gLapicVirtAddr + offset);
}

namespace {

void kSendIcr(kernel::uint32_t destApicId, kernel::uint32_t commandLow) {
    if (gUseX2Apic) {
        // x2APIC: 목적지(전체 32비트) + 명령을 한 번의 64비트 MSR
        // 쓰기로 보낸다 - 스펙상 항상 동기적으로 완료되어 xAPIC의
        // delivery status 폴링이 필요 없다.
        kWriteMsr(kX2ApicIcrMsr, (static_cast<kernel::uint64_t>(destApicId) << 32) | commandLow);
        return;
    }
    // xAPIC: 목적지를 먼저 ICR_HIGH에 쓰고(8비트, 물리모드), ICR_LOW를
    // 쓰는 순간 실제로 IPI가 나간다 - 그래서 순서가 중요하다.
    Lapic::writeRegister(kLapicRegIcrHigh, destApicId << kXApicIcrHighDestShift);
    Lapic::writeRegister(kLapicRegIcrLow, commandLow);
    while (Lapic::readRegister(kLapicRegIcrLow) & kIcrDeliveryStatusBit) {
        asm volatile("pause");
    }
}

}  // namespace

void Lapic::sendInitIpi(kernel::uint32_t destApicId, bool assert) {
    kernel::uint32_t command = kIcrDeliveryModeInit | kIcrTriggerModeLevel;
    if (assert) {
        command |= kIcrLevelAssert;
    }
    kSendIcr(destApicId, command);
}

void Lapic::sendStartupIpi(kernel::uint32_t destApicId, kernel::uint32_t startupVector) {
    const kernel::uint32_t command = kIcrDeliveryModeStartup | (startupVector & 0xFF);
    kSendIcr(destApicId, command);
}

void Lapic::startPeriodicTimer(kernel::uint32_t vector, kernel::uint32_t hz) {
    // ticksPerWindow는 kCalibrationMs(고정 보정 창) 동안의 LAPIC 틱
    // 수다 - 원하는 주기(1000/hz ms)에 맞는 initial count로 환산한다.
    const kernel::uint32_t ticksPerWindow = kCalibrateLapicTicksPerWindow();
    const kernel::uint32_t ticksPerPeriod =
        static_cast<kernel::uint32_t>((static_cast<kernel::uint64_t>(ticksPerWindow) * 1000UL) / (kCalibrationMs * hz));

    writeRegister(kLapicRegDivideConfig, kLapicDivideBy16);
    writeRegister(kLapicRegLvtTimer, vector | kLapicLvtPeriodicBit);
    writeRegister(kLapicRegInitialCount, ticksPerPeriod);
}

}  // namespace kernel
