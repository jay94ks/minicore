#include "lapic.h"

#include "x86_64/io_port.h"
#include "paging.h"

namespace {

constexpr unsigned long kIa32ApicBaseMsr = 0x1B;
constexpr unsigned long kApicBaseEnableBit = 1UL << 11;
constexpr unsigned long kApicBaseExtdBit = 1UL << 10;  // x2APIC 모드 전환 비트
constexpr unsigned long kApicBaseAddrMask = 0x000FFFFFFFFFF000UL;

constexpr unsigned int kRegisterEoi = 0x0B0;
constexpr unsigned int kRegisterSpuriousVector = 0x0F0;
constexpr unsigned int kSpuriousSoftwareEnableBit = 1U << 8;

// LAPIC MMIO는 direct map(WB 캐시)에 그대로 얹으면 안 된다 - 전용
// 가상주소에 캐시 비활성으로 따로 매핑한다. (x2APIC 모드에서는 MMIO
// 매핑 자체를 안 쓴다 - 전부 MSR 접근이라 필요 없음.)
constexpr unsigned long kLapicVirtBase = 0xFFFF901000000000UL;

// x2APIC 레지스터는 MSR 0x800 + (MMIO 오프셋 >> 4)로 접근한다(Intel
// SDM Vol.3 10.12.1) - ID 레지스터(MMIO 0x020)는 MSR 0x802.
constexpr unsigned int kX2ApicMsrBase = 0x800;
constexpr unsigned int kX2ApicIdMsr = 0x802;

unsigned long gLapicVirtAddr = 0;
// xAPIC은 gLapicVirtAddr(!=0)로 준비 여부를 판단했지만, x2APIC은 MMIO
// 매핑이 아예 없어 그 값이 0으로 남는다 - 그래서 준비 플래그를 따로
// 둔다(PageFrameAllocator가 kCurrentNumaNode()에서 물어보는 대상).
bool gLapicReady = false;
// CPUID.01H:ECX 비트21로 감지한 x2APIC 지원 여부 + 실제 그 모드로
// 전환했는지 - init() 시작 시 한 번만 결정하고 이후 고정한다(부팅
// 중 xAPIC<->x2APIC을 오가는 경로는 지원하지 않음, DC 없음: PL-D65F49CC
// 설계 범위 내 결정).
bool gUseX2Apic = false;

// CPUID.01H:ECX 비트21 - x2APIC 지원 여부. ebx는 그냥 버리지만 cpuid는
// eax/ebx/ecx/edx를 전부 건드리므로 전부 출력 제약에 넣어야 한다.
bool kCpuidHasX2Apic() {
    unsigned int eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    constexpr unsigned int kX2ApicCpuidBit = 1U << 21;
    return (ecx & kX2ApicCpuidBit) != 0;
}

// PIC을 CPU 예외 벡터(0-31)와 절대 안 겹치는 곳으로 옮겨 둔다(0xE0
// 마스터/0xE8 슬레이브 - kTimerVector=32, spurious=0xFF와도 안
// 겹치게 골랐다). 완전히 마스크해서 정상적으로는 절대 안 쓰지만,
// 리매핑까지 해 두는 이유는 마스크돼 있어도 드물게 spurious PIC
// 인터럽트(레거시 IRQ7/15)가 새어 나올 수 있기 때문이다 - 리매핑을
// 안 해 두면 그게 기본 벡터(0x08=더블폴트 등)로 들어와 진짜 CPU
// 예외로 오진될 수 있다(설계자 지적, 2026-09-14).
constexpr unsigned char kPicMasterVectorBase = 0xE0;
constexpr unsigned char kPicSlaveVectorBase = 0xE8;

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

unsigned long kReadMsr(unsigned long msr) {
    unsigned int low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<unsigned long>(high) << 32) | low;
}

void kWriteMsr(unsigned long msr, unsigned long value) {
    const auto low = static_cast<unsigned int>(value & 0xFFFFFFFF);
    const auto high = static_cast<unsigned int>(value >> 32);
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

}  // namespace

namespace kernel {

void Lapic::init() {
    kDisableLegacyPic();

    gUseX2Apic = kCpuidHasX2Apic();
    unsigned long apicBaseMsr = kReadMsr(kIa32ApicBaseMsr);

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
        const unsigned long lapicPhysAddr = apicBaseMsr & kApicBaseAddrMask;
        Paging::mapPage(kLapicVirtBase, lapicPhysAddr, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
        gLapicVirtAddr = kLapicVirtBase;
    }

    gLapicReady = true;

    // 소프트웨어 활성화 + spurious 인터럽트 벡터(0xFF, 관례상 흔히
    // 쓰는 값 - 하위 4비트가 전부 1이라 우선순위 그룹 규칙과도 맞음).
    writeRegister(kRegisterSpuriousVector, kSpuriousSoftwareEnableBit | 0xFF);
}

unsigned int Lapic::id() {
    if (gUseX2Apic) {
        // x2APIC ID 레지스터는 MSR 하위 32비트에 ID가 그대로 들어있다
        // (xAPIC MMIO처럼 상위 8비트로 시프트되어 있지 않음 - 8비트
        // 제한도 없어져 32비트 전체를 ID로 쓸 수 있다).
        return static_cast<unsigned int>(kReadMsr(kX2ApicIdMsr));
    }
    return readRegister(0x020) >> 24;
}

bool Lapic::isReady() {
    return gLapicReady;
}

bool Lapic::usesX2Apic() {
    return gUseX2Apic;
}

void Lapic::sendEoi() {
    writeRegister(kRegisterEoi, 0);
}

void Lapic::writeRegister(unsigned int offset, unsigned int value) {
    if (gUseX2Apic) {
        kWriteMsr(kX2ApicMsrBase + (offset >> 4), value);
        return;
    }
    *reinterpret_cast<volatile unsigned int*>(gLapicVirtAddr + offset) = value;
}

unsigned int Lapic::readRegister(unsigned int offset) {
    if (gUseX2Apic) {
        return static_cast<unsigned int>(kReadMsr(kX2ApicMsrBase + (offset >> 4)));
    }
    return *reinterpret_cast<volatile unsigned int*>(gLapicVirtAddr + offset);
}

}  // namespace kernel
