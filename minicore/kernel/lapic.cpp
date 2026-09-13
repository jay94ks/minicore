#include "lapic.h"

#include "x86_64/io_port.h"
#include "paging.h"

namespace {

constexpr unsigned long kIa32ApicBaseMsr = 0x1B;
constexpr unsigned long kApicBaseEnableBit = 1UL << 11;
constexpr unsigned long kApicBaseAddrMask = 0x000FFFFFFFFFF000UL;

constexpr unsigned int kRegisterEoi = 0x0B0;
constexpr unsigned int kRegisterSpuriousVector = 0x0F0;
constexpr unsigned int kSpuriousSoftwareEnableBit = 1U << 8;

// LAPIC MMIO는 direct map(WB 캐시)에 그대로 얹으면 안 된다 - 전용
// 가상주소에 캐시 비활성으로 따로 매핑한다.
constexpr unsigned long kLapicVirtBase = 0xFFFF901000000000UL;

unsigned long gLapicVirtAddr = 0;

void kDisableLegacyPic() {
    // 이 프로젝트는 PIC이 아니라 LAPIC/IOAPIC을 쓴다(설계자 지시 -
    // SMP 고려) - 마스터/슬레이브 PIC의 모든 IRQ 라인을 마스크해
    // 레거시 인터럽트가 끼어들지 못하게 한다. 리매핑조차 안 하는
    // 이유: 이 커널은 PIC 벡터를 절대 쓰지 않을 것이므로 굳이 벡터
    // 충돌을 피할 필요가 없다 - 그냥 완전히 죽여 둔다.
    kernel::kOutB(0x21, 0xFF);
    kernel::kOutB(0xA1, 0xFF);
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

    unsigned long apicBaseMsr = kReadMsr(kIa32ApicBaseMsr);
    if (!(apicBaseMsr & kApicBaseEnableBit)) {
        apicBaseMsr |= kApicBaseEnableBit;
        kWriteMsr(kIa32ApicBaseMsr, apicBaseMsr);
    }
    const unsigned long lapicPhysAddr = apicBaseMsr & kApicBaseAddrMask;

    Paging::mapPage(kLapicVirtBase, lapicPhysAddr, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
    gLapicVirtAddr = kLapicVirtBase;

    // 소프트웨어 활성화 + spurious 인터럽트 벡터(0xFF, 관례상 흔히
    // 쓰는 값 - 하위 4비트가 전부 1이라 우선순위 그룹 규칙과도 맞음).
    writeRegister(kRegisterSpuriousVector, kSpuriousSoftwareEnableBit | 0xFF);
}

unsigned int Lapic::id() {
    return readRegister(0x020) >> 24;
}

void Lapic::sendEoi() {
    writeRegister(kRegisterEoi, 0);
}

void Lapic::writeRegister(unsigned int offset, unsigned int value) {
    *reinterpret_cast<volatile unsigned int*>(gLapicVirtAddr + offset) = value;
}

unsigned int Lapic::readRegister(unsigned int offset) {
    return *reinterpret_cast<volatile unsigned int*>(gLapicVirtAddr + offset);
}

}  // namespace kernel
