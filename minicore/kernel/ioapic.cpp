#include "ioapic.h"

#include "acpi.h"
#include "paging.h"

namespace {

constexpr unsigned int kRegIoRegSel = 0x00;
constexpr unsigned int kRegIoWin = 0x10;
constexpr unsigned int kRegIoApicVer = 0x01;
constexpr unsigned int kRegRedTblBase = 0x10;  // 엔트리당 2개(저/고 32비트): 0x10+2*gsi, 0x10+2*gsi+1

constexpr unsigned int kRedTblMaskedBit = 1U << 16;
constexpr unsigned int kRedTblPolarityBit = 1U << 13;
constexpr unsigned int kRedTblTriggerBit = 1U << 15;
constexpr unsigned int kRedTblDestShift = 24;  // 상위(high) dword 안에서의 오프셋
constexpr unsigned int kRedTblDestMask = 0xFF;  // IOAPIC REDTBL 물리 목적지 필드는 하드웨어 스펙상 8비트

constexpr unsigned int kIoApicVerMaxRedirEntryShift = 16;
constexpr unsigned int kIoApicVerMaxRedirEntryMask = 0xFF;

// LAPIC MMIO(lapic.cpp의 kLapicVirtBase, 0xFFFF901000000000UL) 바로
// 다음 4KiB 페이지 - LAPIC과 서로 다른 물리 프레임을 가리키므로 독립된
// 가상주소가 필요하다. 두 매핑 다 PAGE_CACHE_DISABLE 4KiB MMIO
// 페이지라 이렇게 나란히 둬도 안전하다(page table 상 서로 다른 PTE).
constexpr unsigned long kIoApicVirtBase = 0xFFFF901000001000UL;

unsigned long gIoApicVirtAddr = 0;
unsigned int gMaxRedirectionEntry = 0;  // IOAPICVER에서 읽은 "최대 인덱스"(엔트리 수 - 1)

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
    gMaxRedirectionEntry = (kReadReg(kRegIoApicVer) >> kIoApicVerMaxRedirEntryShift) & kIoApicVerMaxRedirEntryMask;
}

bool IoApic::setRedirection(unsigned int gsi, unsigned int vector, unsigned int destApicId, unsigned int polarity,
                             unsigned int triggerMode) {
    if (gsi > gMaxRedirectionEntry) {
        return false;  // 이 IOAPIC이 아예 갖고 있지 않은 엔트리
    }
    if (destApicId > kRedTblDestMask) {
        // IOAPIC REDTBL의 물리 목적지 필드는 8비트 고정(하드웨어
        // 스펙) - 이걸 넘는 코어는 이 경로로 절대 못 보낸다(조용히
        // 하위 8비트만 잘라 엉뚱한 코어로 보내는 대신 실패를 알린다).
        return false;
    }

    unsigned int low = vector & 0xFF;
    if (polarity == kAcpiPolarityActiveLow) {
        low |= kRedTblPolarityBit;
    }
    if (triggerMode == kAcpiTriggerLevel) {
        low |= kRedTblTriggerBit;
    }
    const unsigned int high = destApicId << kRedTblDestShift;
    const unsigned int regLow = kRegRedTblBase + gsi * 2;
    const unsigned int regHigh = regLow + 1;

    kWriteReg(regHigh, high);
    kWriteReg(regLow, low);
    return true;
}

bool IoApic::setRedirectionForIsaIrq(unsigned int isaIrq, unsigned int vector, unsigned int destApicId) {
    const Acpi::IsaIrqRouting routing = Acpi::resolveIsaIrq(isaIrq);
    return setRedirection(routing.gsi, vector, destApicId, routing.polarity, routing.triggerMode);
}

void IoApic::mask(unsigned int gsi) {
    const unsigned int regLow = kRegRedTblBase + gsi * 2;
    kWriteReg(regLow, kReadReg(regLow) | kRedTblMaskedBit);
}

void IoApic::unmask(unsigned int gsi) {
    const unsigned int regLow = kRegRedTblBase + gsi * 2;
    kWriteReg(regLow, kReadReg(regLow) & ~kRedTblMaskedBit);
}

}  // namespace kernel
