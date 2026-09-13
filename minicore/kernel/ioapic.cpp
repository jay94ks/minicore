#include "ioapic.h"

#include "acpi.h"
#include "paging.h"

namespace {

constexpr unsigned int kRegIoRegSel = 0x00;
constexpr unsigned int kRegIoWin = 0x10;
constexpr unsigned int kRegIoApicVer = 0x01;
constexpr unsigned int kRegRedTblBase = 0x10;  // 엔트리당 2개(저/고 32비트): 0x10+2*localIndex, 0x10+2*localIndex+1

constexpr unsigned int kRedTblMaskedBit = 1U << 16;
constexpr unsigned int kRedTblPolarityBit = 1U << 13;
constexpr unsigned int kRedTblTriggerBit = 1U << 15;
constexpr unsigned int kRedTblDestShift = 24;  // 상위(high) dword 안에서의 오프셋
constexpr unsigned int kRedTblDestMask = 0xFF;  // IOAPIC REDTBL 물리 목적지 필드는 하드웨어 스펙상 8비트

constexpr unsigned int kIoApicVerMaxRedirEntryShift = 16;
constexpr unsigned int kIoApicVerMaxRedirEntryMask = 0xFF;

// LAPIC MMIO(lapic.cpp의 kLapicVirtBase, 0xFFFF901000000000UL) 바로
// 다음 4KiB 페이지부터 IOAPIC 인스턴스마다 한 페이지씩 순서대로 쓴다 -
// MMCONFIG(0xFFFF901000010000UL)가 시작되기 전까지 kAcpiMaxIoApics
// (8)개를 위한 여유가 충분하다(0x1000~0x8000, MMCONFIG는 0x10000부터).
constexpr unsigned long kIoApicVirtBase = 0xFFFF901000001000UL;

struct IoApicInstance {
    unsigned long virtAddr;
    unsigned int gsiBase;
    unsigned int maxRedirectionEntry;  // IOAPICVER에서 읽은 "최대 인덱스"(엔트리 수 - 1)
};

IoApicInstance gInstances[kernel::kAcpiMaxIoApics];
unsigned int gInstanceCount = 0;

unsigned int kReadReg(const IoApicInstance& instance, unsigned int reg) {
    *reinterpret_cast<volatile unsigned int*>(instance.virtAddr + kRegIoRegSel) = reg;
    return *reinterpret_cast<volatile unsigned int*>(instance.virtAddr + kRegIoWin);
}

void kWriteReg(const IoApicInstance& instance, unsigned int reg, unsigned int value) {
    *reinterpret_cast<volatile unsigned int*>(instance.virtAddr + kRegIoRegSel) = reg;
    *reinterpret_cast<volatile unsigned int*>(instance.virtAddr + kRegIoWin) = value;
}

// gsi를 담당하는 인스턴스를 찾아 그 안에서의 지역 리다이렉션 인덱스를
// outLocalIndex로 돌려준다 - 어느 인스턴스에도 안 속하면 nullptr.
const IoApicInstance* kFindInstanceForGsi(unsigned int gsi, unsigned int* outLocalIndex) {
    for (unsigned int i = 0; i < gInstanceCount; ++i) {
        const IoApicInstance& instance = gInstances[i];
        if (gsi >= instance.gsiBase && gsi - instance.gsiBase <= instance.maxRedirectionEntry) {
            *outLocalIndex = gsi - instance.gsiBase;
            return &instance;
        }
    }
    return nullptr;
}

}  // namespace

namespace kernel {

void IoApic::init() {
    const unsigned int count = Acpi::ioApicCount();
    gInstanceCount = count < kAcpiMaxIoApics ? count : kAcpiMaxIoApics;

    for (unsigned int i = 0; i < gInstanceCount; ++i) {
        const unsigned long virtAddr = kIoApicVirtBase + static_cast<unsigned long>(i) * 4096UL;
        Paging::mapPage(virtAddr, Acpi::ioApicAddress(i), PAGE_WRITABLE | PAGE_CACHE_DISABLE);

        gInstances[i].virtAddr = virtAddr;
        gInstances[i].gsiBase = Acpi::ioApicGsiBase(i);
        gInstances[i].maxRedirectionEntry =
            (kReadReg(gInstances[i], kRegIoApicVer) >> kIoApicVerMaxRedirEntryShift) & kIoApicVerMaxRedirEntryMask;
    }
}

bool IoApic::setRedirection(unsigned int gsi, unsigned int vector, unsigned int destApicId, unsigned int polarity,
                             unsigned int triggerMode) {
    unsigned int localIndex = 0;
    const IoApicInstance* instance = kFindInstanceForGsi(gsi, &localIndex);
    if (!instance) {
        return false;  // 이 GSI를 담당하는 IOAPIC이 없음
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
    const unsigned int regLow = kRegRedTblBase + localIndex * 2;
    const unsigned int regHigh = regLow + 1;

    kWriteReg(*instance, regHigh, high);
    kWriteReg(*instance, regLow, low);
    return true;
}

bool IoApic::setRedirectionForIsaIrq(unsigned int isaIrq, unsigned int vector, unsigned int destApicId) {
    const Acpi::IsaIrqRouting routing = Acpi::resolveIsaIrq(isaIrq);
    return setRedirection(routing.gsi, vector, destApicId, routing.polarity, routing.triggerMode);
}

void IoApic::mask(unsigned int gsi) {
    unsigned int localIndex = 0;
    const IoApicInstance* instance = kFindInstanceForGsi(gsi, &localIndex);
    if (!instance) {
        return;
    }
    const unsigned int regLow = kRegRedTblBase + localIndex * 2;
    kWriteReg(*instance, regLow, kReadReg(*instance, regLow) | kRedTblMaskedBit);
}

void IoApic::unmask(unsigned int gsi) {
    unsigned int localIndex = 0;
    const IoApicInstance* instance = kFindInstanceForGsi(gsi, &localIndex);
    if (!instance) {
        return;
    }
    const unsigned int regLow = kRegRedTblBase + localIndex * 2;
    kWriteReg(*instance, regLow, kReadReg(*instance, regLow) & ~kRedTblMaskedBit);
}

}  // namespace kernel
