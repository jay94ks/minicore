#include "pci.h"

#include "acpi.h"
#include "libkenv/types.h"
#include "paging.h"
#include "x86_64/io_port.h"

namespace {

constexpr kernel::uint16_t kConfigAddressPort = 0xCF8;
constexpr kernel::uint16_t kConfigDataPort = 0xCFC;
constexpr kernel::uint32_t kConfigAddressEnableBit = 1U << 31;

constexpr kernel::uint8_t kOffsetVendorId = 0x00;
constexpr kernel::uint8_t kOffsetDeviceId = 0x02;
constexpr kernel::uint8_t kOffsetCommand = 0x04;
constexpr kernel::uint8_t kOffsetStatus = 0x06;
constexpr kernel::uint8_t kOffsetRevisionId = 0x08;
constexpr kernel::uint8_t kOffsetProgIf = 0x09;
constexpr kernel::uint8_t kOffsetSubclass = 0x0A;
constexpr kernel::uint8_t kOffsetClassCode = 0x0B;
constexpr kernel::uint8_t kOffsetHeaderType = 0x0E;
constexpr kernel::uint8_t kOffsetBar0 = 0x10;
constexpr kernel::uint8_t kOffsetSecondaryBus = 0x19;
constexpr kernel::uint8_t kOffsetCapabilitiesPointer = 0x34;

constexpr kernel::uint16_t kStatusCapabilitiesListBit = 1U << 4;
constexpr kernel::uint8_t kHeaderTypeMultiFunctionBit = 0x80;
constexpr kernel::uint8_t kHeaderTypeMask = 0x7F;

constexpr kernel::uint8_t kClassBridge = 0x06;
constexpr kernel::uint8_t kSubclassPciToPciBridge = 0x04;

constexpr kernel::uint8_t kCapabilityIdMsi = 0x05;
constexpr kernel::uint8_t kCapabilityIdMsix = 0x11;

constexpr kernel::uint16_t kMsiControlEnableBit = 1U << 0;
constexpr kernel::uint16_t kMsiControl64BitCapableBit = 1U << 7;
constexpr kernel::uint16_t kMsiControlMultiMessageEnableShift = 4;
constexpr kernel::uint16_t kMsiControlMultiMessageEnableMask = 0x7U << kMsiControlMultiMessageEnableShift;
constexpr kernel::uint16_t kMsiControlMultiMessageCapableShift = 1;
constexpr kernel::uint16_t kMsiControlMultiMessageCapableMask = 0x7U << kMsiControlMultiMessageCapableShift;

constexpr kernel::uint16_t kMsixControlTableSizeMask = 0x7FF;  // bits0-10, 값=테이블크기-1
constexpr kernel::uint16_t kMsixControlFunctionMaskBit = 1U << 14;
constexpr kernel::uint16_t kMsixControlEnableBit = 1U << 15;
constexpr kernel::uint32_t kMsixBirMask = 0x7;
constexpr kernel::uint32_t kMsixOffsetMask = ~0x7U;
constexpr kernel::uint64_t kMsixTableEntrySize = 16;
constexpr kernel::uint32_t kMsixVectorControlMaskedBit = 1U << 0;

constexpr kernel::uint16_t kVendorIdNoDevice = 0xFFFF;

constexpr kernel::uint16_t kCommandMemorySpaceBit = 1U << 1;
constexpr kernel::uint16_t kCommandBusMasterBit = 1U << 2;

// MMCONFIG(ECAM) 전용 가상주소 - LAPIC/IOAPIC/HPET 다음 슬롯들과
// 겹치지 않게 별도로 크게 떼어 뒀다(bus 0 하나치, 32개 장치 x 8
// 함수 x 4KiB = 1MiB).
constexpr kernel::uint64_t kMmconfigBus0VirtBase = 0xFFFF901000010000UL;
constexpr kernel::uint64_t kMmconfigPerFunctionSize = 4096;
constexpr kernel::uint64_t kMmconfigBus0Size = 32 * 8 * kMmconfigPerFunctionSize;  // 1MiB

bool gUseMmconfigBus0 = false;
kernel::uint64_t gMcfgBusOffset = 0;  // gMcfgBusOffset = bus0의 ECAM 물리 시작 주소(= McfgBase, startBus==0일 때)

kernel::uint32_t kLegacyConfigAddress(kernel::uint8_t bus, kernel::uint8_t device, kernel::uint8_t function, kernel::uint8_t offset) {
    return kConfigAddressEnableBit | (static_cast<kernel::uint32_t>(bus) << 16) |
           (static_cast<kernel::uint32_t>(device & 0x1F) << 11) | (static_cast<kernel::uint32_t>(function & 0x07) << 8) |
           (offset & 0xFC);
}

// bus 0 + MMCONFIG 사용 가능 여부 - 이 조건일 때만 ECAM 경로를 쓰고,
// 그 외(다른 버스, 또는 MCFG 자체가 없음)는 전부 레거시로 폴백한다.
bool kShouldUseMmconfig(kernel::uint8_t bus) { return gUseMmconfigBus0 && bus == 0; }

kernel::uint64_t kMmconfigVirtAddress(kernel::uint8_t device, kernel::uint8_t function, kernel::uint8_t offset) {
    return kMmconfigBus0VirtBase + (static_cast<kernel::uint64_t>(device) * 8 + function) * kMmconfigPerFunctionSize +
           offset;
}

void kScanBus(kernel::uint8_t bus, kernel::Pci::EnumerateCallback callback);

void kScanFunction(kernel::uint8_t bus, kernel::uint8_t device, kernel::uint8_t function, kernel::Pci::EnumerateCallback callback) {
    const kernel::uint16_t vendorId = kernel::Pci::readConfig16(bus, device, function, kOffsetVendorId);
    if (vendorId == kVendorIdNoDevice) {
        return;
    }

    kernel::Pci::Device dev;
    dev.bus = bus;
    dev.device = device;
    dev.function = function;
    dev.vendorId = vendorId;
    dev.deviceId = kernel::Pci::readConfig16(bus, device, function, kOffsetDeviceId);
    dev.revisionId = kernel::Pci::readConfig8(bus, device, function, kOffsetRevisionId);
    dev.progIf = kernel::Pci::readConfig8(bus, device, function, kOffsetProgIf);
    dev.subclass = kernel::Pci::readConfig8(bus, device, function, kOffsetSubclass);
    dev.classCode = kernel::Pci::readConfig8(bus, device, function, kOffsetClassCode);
    dev.headerType = kernel::Pci::readConfig8(bus, device, function, kOffsetHeaderType) & kHeaderTypeMask;

    callback(dev);

    if (dev.classCode == kClassBridge && dev.subclass == kSubclassPciToPciBridge) {
        const kernel::uint8_t secondaryBus = kernel::Pci::readConfig8(bus, device, function, kOffsetSecondaryBus);
        kScanBus(secondaryBus, callback);
    }
}

void kScanBus(kernel::uint8_t bus, kernel::Pci::EnumerateCallback callback) {
    for (kernel::uint32_t device = 0; device < 32; ++device) {
        const kernel::uint16_t vendorId = kernel::Pci::readConfig16(bus, static_cast<kernel::uint8_t>(device), 0, kOffsetVendorId);
        if (vendorId == kVendorIdNoDevice) {
            continue;
        }
        const kernel::uint8_t headerType = kernel::Pci::readConfig8(bus, static_cast<kernel::uint8_t>(device), 0, kOffsetHeaderType);
        const kernel::uint32_t functionCount = (headerType & kHeaderTypeMultiFunctionBit) ? 8 : 1;
        for (kernel::uint32_t function = 0; function < functionCount; ++function) {
            kScanFunction(bus, static_cast<kernel::uint8_t>(device), static_cast<kernel::uint8_t>(function), callback);
        }
    }
}

// BAR(offset 0x10 + 4*barIndex)의 물리 베이스 주소를 읽는다 - 64비트
// BAR(type bits01=10b)면 다음 슬롯과 합쳐 64비트 값을 만든다.
kernel::uint64_t kReadBarAddress(kernel::uint8_t bus, kernel::uint8_t device, kernel::uint8_t function, kernel::uint32_t barIndex) {
    const auto barOffset = static_cast<kernel::uint8_t>(kOffsetBar0 + barIndex * 4);
    const kernel::uint32_t low = kernel::Pci::readConfig32(bus, device, function, barOffset);
    constexpr kernel::uint32_t kBarTypeMask = 0x6;
    constexpr kernel::uint32_t kBarType64Bit = 0x4;
    kernel::uint64_t address = low & ~0xFUL;
    if ((low & kBarTypeMask) == kBarType64Bit) {
        const kernel::uint32_t high = kernel::Pci::readConfig32(bus, device, function, static_cast<kernel::uint8_t>(barOffset + 4));
        address |= static_cast<kernel::uint64_t>(high) << 32;
    }
    return address;
}

}  // namespace

namespace kernel {

void Pci::init() {
    if (!Acpi::hasMcfg()) {
        return;
    }
    if (Acpi::mcfgStartBus() != 0 || Acpi::mcfgEndBus() == 0) {
        return;  // bus 0이 이 세그먼트에 없음 - 레거시로만 동작(드문 구성, 관계도에 기록)
    }
    gMcfgBusOffset = Acpi::mcfgBaseAddress();
    Paging::mapPage(kMmconfigBus0VirtBase, gMcfgBusOffset, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
    for (uint64_t off = kMmconfigPerFunctionSize; off < kMmconfigBus0Size; off += kMmconfigPerFunctionSize) {
        Paging::mapPage(kMmconfigBus0VirtBase + off, gMcfgBusOffset + off, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
    }
    gUseMmconfigBus0 = true;
}

bool Pci::usesMmconfig() { return gUseMmconfigBus0; }

uint32_t Pci::readConfig32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    if (kShouldUseMmconfig(bus)) {
        return *reinterpret_cast<volatile uint32_t*>(kMmconfigVirtAddress(device, function, offset));
    }
    arch::kOutL(kConfigAddressPort, kLegacyConfigAddress(bus, device, function, offset));
    return arch::kInL(kConfigDataPort);
}

void Pci::writeConfig32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    if (kShouldUseMmconfig(bus)) {
        *reinterpret_cast<volatile uint32_t*>(kMmconfigVirtAddress(device, function, offset)) = value;
        return;
    }
    arch::kOutL(kConfigAddressPort, kLegacyConfigAddress(bus, device, function, offset));
    arch::kOutL(kConfigDataPort, value);
}

// CONFIG_DATA(0xCFC)에 오프셋의 하위 2비트를 더한 포트로 접근하면
// 칩셋이 해당 폭(8/16/32비트)의 바이트 레인만 골라준다 - 32비트로
// 통째로 읽어 마스킹/시프트하는 것보다 실제 하드웨어 동작과 일치하고
// 더 간단하다. MMCONFIG는 그 자체가 일반 MMIO라 폭에 맞는 포인터
// 타입으로 그냥 읽고/쓰면 된다.
uint16_t Pci::readConfig16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    if (kShouldUseMmconfig(bus)) {
        return *reinterpret_cast<volatile uint16_t*>(kMmconfigVirtAddress(device, function, offset));
    }
    arch::kOutL(kConfigAddressPort, kLegacyConfigAddress(bus, device, function, offset));
    return arch::kInW(static_cast<uint16_t>(kConfigDataPort + (offset & 2)));
}

void Pci::writeConfig16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    if (kShouldUseMmconfig(bus)) {
        *reinterpret_cast<volatile uint16_t*>(kMmconfigVirtAddress(device, function, offset)) = value;
        return;
    }
    arch::kOutL(kConfigAddressPort, kLegacyConfigAddress(bus, device, function, offset));
    arch::kOutW(static_cast<uint16_t>(kConfigDataPort + (offset & 2)), value);
}

uint8_t Pci::readConfig8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    if (kShouldUseMmconfig(bus)) {
        return *reinterpret_cast<volatile uint8_t*>(kMmconfigVirtAddress(device, function, offset));
    }
    arch::kOutL(kConfigAddressPort, kLegacyConfigAddress(bus, device, function, offset));
    return arch::kInB(static_cast<uint16_t>(kConfigDataPort + (offset & 3)));
}

void Pci::enumerate(EnumerateCallback callback) {
    kScanBus(0, callback);
}

uint8_t Pci::findCapability(uint8_t bus, uint8_t device, uint8_t function, uint8_t capabilityId) {
    const uint16_t status = readConfig16(bus, device, function, kOffsetStatus);
    if (!(status & kStatusCapabilitiesListBit)) {
        return 0;
    }

    uint8_t ptr = readConfig8(bus, device, function, kOffsetCapabilitiesPointer) & 0xFC;
    while (ptr != 0) {
        const uint8_t id = readConfig8(bus, device, function, ptr);
        if (id == capabilityId) {
            return ptr;
        }
        ptr = readConfig8(bus, device, function, static_cast<uint8_t>(ptr + 1)) & 0xFC;
    }
    return 0;
}

bool Pci::enableMsi(uint8_t bus, uint8_t device, uint8_t function, uint32_t vector, uint32_t destApicId) {
    uint32_t grantedBase = 0;
    uint32_t grantedCount = 0;
    if (!enableMsiVectors(bus, device, function, 1, vector, destApicId, &grantedBase, &grantedCount)) {
        return false;
    }
    return grantedCount >= 1;
}

bool Pci::enableMsiVectors(uint8_t bus, uint8_t device, uint8_t function, uint32_t requestedCount,
                            uint32_t preferredBase, uint32_t destApicId, uint32_t* outBaseVector,
                            uint32_t* outGrantedCount) {
    *outBaseVector = 0;
    *outGrantedCount = 0;

    const uint8_t msiOffset = findCapability(bus, device, function, kCapabilityIdMsi);
    if (!msiOffset) {
        return false;
    }
    if (destApicId > 0xFF) {
        // 클래식 MSI Message Address의 물리 목적지 필드는 IOAPIC
        // REDTBL과 마찬가지로 8비트 고정이다(SDM Vol.3 10.11.1) -
        // 조용히 자르지 않고 실패를 알린다(ioapic.cpp와 같은 원칙).
        return false;
    }

    uint16_t messageControl = readConfig16(bus, device, function, static_cast<uint8_t>(msiOffset + 2));
    const bool is64BitCapable = (messageControl & kMsiControl64BitCapableBit) != 0;
    const uint32_t multiMessageCapableLog2 =
        (messageControl & kMsiControlMultiMessageCapableMask) >> kMsiControlMultiMessageCapableShift;
    const uint32_t deviceMaxVectors = 1U << multiMessageCapableLog2;

    // requestedCount를 2의 거듭제곱으로 내림하고 장치 한도로 다시
    // 제한한다(MSI 하드웨어 자체가 2의 거듭제곱 개수만 지원).
    uint32_t grantedCount = 1;
    while (grantedCount * 2 <= requestedCount && grantedCount * 2 <= deviceMaxVectors) {
        grantedCount *= 2;
    }
    // preferredBase를 grantedCount의 배수로 내림(MSI는 벡터 그룹이
    // 그 개수만큼 정렬돼 있어야 한다 - 하드웨어가 데이터 레지스터의
    // 하위 로그2(count)비트를 인터럽트 인덱스로 자동 채우는 방식이라).
    const uint32_t baseVector = preferredBase & ~(grantedCount - 1);

    const uint32_t messageAddress = 0xFEE00000U | ((destApicId & 0xFF) << 12);
    const auto messageData = static_cast<uint16_t>(baseVector & 0xFF);

    writeConfig32(bus, device, function, static_cast<uint8_t>(msiOffset + 4), messageAddress);
    if (is64BitCapable) {
        writeConfig32(bus, device, function, static_cast<uint8_t>(msiOffset + 8), 0);  // 상위 32비트 - LAPIC은 항상 4GiB 이하
        writeConfig16(bus, device, function, static_cast<uint8_t>(msiOffset + 12), messageData);
    } else {
        writeConfig16(bus, device, function, static_cast<uint8_t>(msiOffset + 8), messageData);
    }

    uint32_t mmeLog2 = 0;
    while ((1U << mmeLog2) < grantedCount) {
        ++mmeLog2;
    }
    messageControl = static_cast<uint16_t>(messageControl & ~kMsiControlMultiMessageEnableMask);
    messageControl = static_cast<uint16_t>(messageControl | (mmeLog2 << kMsiControlMultiMessageEnableShift));
    messageControl = static_cast<uint16_t>(messageControl | kMsiControlEnableBit);
    writeConfig16(bus, device, function, static_cast<uint8_t>(msiOffset + 2), messageControl);

    *outBaseVector = baseVector;
    *outGrantedCount = grantedCount;
    return true;
}

bool Pci::enableMsix(uint8_t bus, uint8_t device, uint8_t function, uint32_t tableIndex,
                      uint32_t vector, uint32_t destApicId) {
    const uint8_t msixOffset = findCapability(bus, device, function, kCapabilityIdMsix);
    if (!msixOffset) {
        return false;
    }

    uint16_t messageControl = readConfig16(bus, device, function, static_cast<uint8_t>(msixOffset + 2));
    const uint32_t tableSize = (messageControl & kMsixControlTableSizeMask) + 1;
    if (tableIndex >= tableSize) {
        return false;
    }

    const uint32_t tableOffsetBir = readConfig32(bus, device, function, static_cast<uint8_t>(msixOffset + 4));
    const uint32_t bir = tableOffsetBir & kMsixBirMask;
    const uint32_t tableOffsetInBar = tableOffsetBir & kMsixOffsetMask;

    const uint64_t barPhys = kReadBarAddress(bus, device, function, bir);
    const uint64_t tablePhys = barPhys + tableOffsetInBar;
    const uint64_t entryPhys = tablePhys + tableIndex * kMsixTableEntrySize;

    // 이 엔트리가 속한 4KiB 페이지 하나만 매핑한다 - 여러 장치/여러
    // 호출이 겹칠 수 있어 매번 같은 임시 가상주소를 재사용(직후에
    // 바로 다 써버리므로 안전 - 영구 매핑이 필요해지면 그때 캐시).
    constexpr uint64_t kMsixTableVirtBase = 0xFFFF901000110000UL;
    const uint64_t pageBase = entryPhys & ~0xFFFUL;
    const uint64_t pageOffset = entryPhys & 0xFFFUL;
    Paging::mapPage(kMsixTableVirtBase, pageBase, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
    auto* entry = reinterpret_cast<volatile uint32_t*>(kMsixTableVirtBase + pageOffset);

    const uint32_t messageAddress = 0xFEE00000U | ((destApicId & 0xFF) << 12);
    entry[0] = messageAddress;  // Message Address Low
    entry[1] = 0;               // Message Address High - LAPIC은 항상 4GiB 이하
    entry[2] = vector & 0xFF;   // Message Data
    entry[3] = entry[3] & ~kMsixVectorControlMaskedBit;  // 이 엔트리 마스크 해제

    const uint16_t command = readConfig16(bus, device, function, kOffsetCommand);
    writeConfig16(bus, device, function, kOffsetCommand,
                  static_cast<uint16_t>(command | kCommandMemorySpaceBit | kCommandBusMasterBit));

    messageControl = static_cast<uint16_t>(messageControl & ~kMsixControlFunctionMaskBit);
    messageControl = static_cast<uint16_t>(messageControl | kMsixControlEnableBit);
    writeConfig16(bus, device, function, static_cast<uint8_t>(msixOffset + 2), messageControl);
    return true;
}

}  // namespace kernel
