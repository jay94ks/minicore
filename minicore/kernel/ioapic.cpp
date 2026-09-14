#include "ioapic.h"

#include "acpi.h"
#include "libkenv/types.h"
#include "paging.h"

namespace {

constexpr kernel::uint32_t kRegIoRegSel = 0x00;
constexpr kernel::uint32_t kRegIoWin = 0x10;
constexpr kernel::uint32_t kRegIoApicVer = 0x01;
constexpr kernel::uint32_t kRegRedTblBase = 0x10;  // 엔트리당 2개(저/고 32비트): 0x10+2*localIndex, 0x10+2*localIndex+1

constexpr kernel::uint32_t kRedTblMaskedBit = 1U << 16;
constexpr kernel::uint32_t kRedTblPolarityBit = 1U << 13;
constexpr kernel::uint32_t kRedTblTriggerBit = 1U << 15;
constexpr kernel::uint32_t kRedTblDestShift = 24;  // 상위(high) dword 안에서의 오프셋
constexpr kernel::uint32_t kRedTblDestMask = 0xFF;  // IOAPIC REDTBL 물리 목적지 필드는 하드웨어 스펙상 8비트

constexpr kernel::uint32_t kIoApicVerMaxRedirEntryShift = 16;
constexpr kernel::uint32_t kIoApicVerMaxRedirEntryMask = 0xFF;

// LAPIC MMIO(lapic.cpp의 kLapicVirtBase, 0xFFFF901000000000UL) 바로
// 다음 4KiB 페이지부터 IOAPIC 인스턴스마다 한 페이지씩 순서대로 쓴다 -
// MMCONFIG(0xFFFF901000010000UL)가 시작되기 전까지 kAcpiMaxIoApics
// (8)개를 위한 여유가 충분하다(0x1000~0x8000, MMCONFIG는 0x10000부터).
constexpr kernel::uint64_t kIoApicVirtBase = 0xFFFF901000001000UL;

struct IoApicInstance {
    kernel::uint64_t virtAddr;
    kernel::uint32_t gsiBase;
    kernel::uint32_t maxRedirectionEntry;  // IOAPICVER에서 읽은 "최대 인덱스"(엔트리 수 - 1)
};

IoApicInstance gInstances[kernel::kAcpiMaxIoApics];
kernel::uint32_t gInstanceCount = 0;

kernel::uint32_t kReadReg(const IoApicInstance& instance, kernel::uint32_t reg) {
    *reinterpret_cast<volatile kernel::uint32_t*>(instance.virtAddr + kRegIoRegSel) = reg;
    return *reinterpret_cast<volatile kernel::uint32_t*>(instance.virtAddr + kRegIoWin);
}

void kWriteReg(const IoApicInstance& instance, kernel::uint32_t reg, kernel::uint32_t value) {
    *reinterpret_cast<volatile kernel::uint32_t*>(instance.virtAddr + kRegIoRegSel) = reg;
    *reinterpret_cast<volatile kernel::uint32_t*>(instance.virtAddr + kRegIoWin) = value;
}

// gsi를 담당하는 인스턴스를 찾아 그 안에서의 지역 리다이렉션 인덱스를
// outLocalIndex로 돌려준다 - 어느 인스턴스에도 안 속하면 nullptr.
const IoApicInstance* kFindInstanceForGsi(kernel::uint32_t gsi, kernel::uint32_t* outLocalIndex) {
    for (kernel::uint32_t i = 0; i < gInstanceCount; ++i) {
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
    const kernel::uint32_t count = Acpi::ioApicCount();
    gInstanceCount = count < kAcpiMaxIoApics ? count : kAcpiMaxIoApics;

    for (kernel::uint32_t i = 0; i < gInstanceCount; ++i) {
        const kernel::uint64_t virtAddr = kIoApicVirtBase + static_cast<kernel::uint64_t>(i) * 4096UL;
        Paging::mapPage(virtAddr, Acpi::ioApicAddress(i), PAGE_WRITABLE | PAGE_CACHE_DISABLE);

        gInstances[i].virtAddr = virtAddr;
        gInstances[i].gsiBase = Acpi::ioApicGsiBase(i);
        gInstances[i].maxRedirectionEntry =
            (kReadReg(gInstances[i], kRegIoApicVer) >> kIoApicVerMaxRedirEntryShift) & kIoApicVerMaxRedirEntryMask;
    }
}

bool IoApic::setRedirection(kernel::uint32_t gsi, kernel::uint32_t vector, kernel::uint32_t destApicId, kernel::uint32_t polarity,
                             kernel::uint32_t triggerMode) {
    kernel::uint32_t localIndex = 0;
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

    kernel::uint32_t low = vector & 0xFF;
    if (polarity == kAcpiPolarityActiveLow) {
        low |= kRedTblPolarityBit;
    }
    if (triggerMode == kAcpiTriggerLevel) {
        low |= kRedTblTriggerBit;
    }
    const kernel::uint32_t high = destApicId << kRedTblDestShift;
    const kernel::uint32_t regLow = kRegRedTblBase + localIndex * 2;
    const kernel::uint32_t regHigh = regLow + 1;

    kWriteReg(*instance, regHigh, high);
    kWriteReg(*instance, regLow, low);
    return true;
}

bool IoApic::setRedirectionForIsaIrq(kernel::uint32_t isaIrq, kernel::uint32_t vector, kernel::uint32_t destApicId) {
    const Acpi::IsaIrqRouting routing = Acpi::resolveIsaIrq(isaIrq);
    return setRedirection(routing.gsi, vector, destApicId, routing.polarity, routing.triggerMode);
}

void IoApic::mask(kernel::uint32_t gsi) {
    kernel::uint32_t localIndex = 0;
    const IoApicInstance* instance = kFindInstanceForGsi(gsi, &localIndex);
    if (!instance) {
        return;
    }
    const kernel::uint32_t regLow = kRegRedTblBase + localIndex * 2;
    kWriteReg(*instance, regLow, kReadReg(*instance, regLow) | kRedTblMaskedBit);
}

void IoApic::unmask(kernel::uint32_t gsi) {
    kernel::uint32_t localIndex = 0;
    const IoApicInstance* instance = kFindInstanceForGsi(gsi, &localIndex);
    if (!instance) {
        return;
    }
    const kernel::uint32_t regLow = kRegRedTblBase + localIndex * 2;
    kWriteReg(*instance, regLow, kReadReg(*instance, regLow) & ~kRedTblMaskedBit);
}

}  // namespace kernel
