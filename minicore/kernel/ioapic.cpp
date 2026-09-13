#include "ioapic.h"

#include "acpi.h"
#include "paging.h"

namespace {

constexpr unsigned int kRegIoRegSel = 0x00;
constexpr unsigned int kRegIoWin = 0x10;
constexpr unsigned int kRegRedTblBase = 0x10;  // 엔트리당 2개(저/고 32비트): 0x10+2*irq, 0x10+2*irq+1

constexpr unsigned int kRedTblMaskedBit = 1U << 16;
constexpr unsigned int kRedTblDestShift = 24;  // 상위(high) dword 안에서의 오프셋

// LAPIC MMIO(lapic.cpp의 kLapicVirtBase, 0xFFFF901000000000UL) 바로
// 다음 4KiB 페이지 - LAPIC과 서로 다른 물리 프레임을 가리키므로 독립된
// 가상주소가 필요하다. 두 매핑 다 PAGE_CACHE_DISABLE 4KiB MMIO
// 페이지라 이렇게 나란히 둬도 안전하다(page table 상 서로 다른 PTE).
constexpr unsigned long kIoApicVirtBase = 0xFFFF901000001000UL;

unsigned long gIoApicVirtAddr = 0;

unsigned int kReadReg(unsigned int reg) {
    *reinterpret_cast<volatile unsigned int*>(gIoApicVirtAddr + kRegIoRegSel) = reg;
    return *reinterpret_cast<volatile unsigned int*>(gIoApicVirtAddr + kRegIoWin);
}

void kWriteReg(unsigned int reg, unsigned int value) {
    *reinterpret_cast<volatile unsigned int*>(gIoApicVirtAddr + kRegIoRegSel) = reg;
    *reinterpret_cast<volatile unsigned int*>(gIoApicVirtAddr + kRegIoWin) = value;
}

}  // namespace

namespace kernel {

void IoApic::init() {
    const unsigned long phys = Acpi::ioApicAddress();
    Paging::mapPage(kIoApicVirtBase, phys, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
    gIoApicVirtAddr = kIoApicVirtBase;
}

void IoApic::setRedirection(unsigned int irq, unsigned int vector, unsigned int destApicId) {
    // low dword: 벡터(0-7) + 델리버리 모드(8-10, 0=Fixed) + 목적지
    // 모드(11, 0=physical) + 극성(13, 0=active-high) + 트리거 모드
    // (15, 0=edge) - 전부 기본값 0이라 vector만 세팅하면 된다(레거시
    // ISA 인터럽트는 관례적으로 active-high/edge).
    const unsigned int low = vector & 0xFF;
    const unsigned int high = (destApicId & 0xFF) << kRedTblDestShift;
    const unsigned int regLow = kRegRedTblBase + irq * 2;
    const unsigned int regHigh = regLow + 1;

    kWriteReg(regHigh, high);
    kWriteReg(regLow, low);
}

void IoApic::mask(unsigned int irq) {
    const unsigned int regLow = kRegRedTblBase + irq * 2;
    kWriteReg(regLow, kReadReg(regLow) | kRedTblMaskedBit);
}

void IoApic::unmask(unsigned int irq) {
    const unsigned int regLow = kRegRedTblBase + irq * 2;
    kWriteReg(regLow, kReadReg(regLow) & ~kRedTblMaskedBit);
}

}  // namespace kernel
