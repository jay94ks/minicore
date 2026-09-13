#include "pci.h"

#include "acpi.h"
#include "paging.h"
#include "x86_64/io_port.h"

namespace {

constexpr unsigned short kConfigAddressPort = 0xCF8;
constexpr unsigned short kConfigDataPort = 0xCFC;
constexpr unsigned int kConfigAddressEnableBit = 1U << 31;

constexpr unsigned char kOffsetVendorId = 0x00;
constexpr unsigned char kOffsetDeviceId = 0x02;
constexpr unsigned char kOffsetCommand = 0x04;
constexpr unsigned char kOffsetStatus = 0x06;
constexpr unsigned char kOffsetRevisionId = 0x08;
constexpr unsigned char kOffsetProgIf = 0x09;
constexpr unsigned char kOffsetSubclass = 0x0A;
constexpr unsigned char kOffsetClassCode = 0x0B;
constexpr unsigned char kOffsetHeaderType = 0x0E;
constexpr unsigned char kOffsetBar0 = 0x10;
constexpr unsigned char kOffsetSecondaryBus = 0x19;
constexpr unsigned char kOffsetCapabilitiesPointer = 0x34;

constexpr unsigned short kStatusCapabilitiesListBit = 1U << 4;
constexpr unsigned char kHeaderTypeMultiFunctionBit = 0x80;
constexpr unsigned char kHeaderTypeMask = 0x7F;

constexpr unsigned char kClassBridge = 0x06;
constexpr unsigned char kSubclassPciToPciBridge = 0x04;

constexpr unsigned char kCapabilityIdMsi = 0x05;
constexpr unsigned char kCapabilityIdMsix = 0x11;

constexpr unsigned short kMsiControlEnableBit = 1U << 0;
constexpr unsigned short kMsiControl64BitCapableBit = 1U << 7;
constexpr unsigned short kMsiControlMultiMessageEnableShift = 4;
constexpr unsigned short kMsiControlMultiMessageEnableMask = 0x7U << kMsiControlMultiMessageEnableShift;
constexpr unsigned short kMsiControlMultiMessageCapableShift = 1;
constexpr unsigned short kMsiControlMultiMessageCapableMask = 0x7U << kMsiControlMultiMessageCapableShift;

constexpr unsigned short kMsixControlTableSizeMask = 0x7FF;  // bits0-10, 값=테이블크기-1
constexpr unsigned short kMsixControlFunctionMaskBit = 1U << 14;
constexpr unsigned short kMsixControlEnableBit = 1U << 15;
constexpr unsigned int kMsixBirMask = 0x7;
constexpr unsigned int kMsixOffsetMask = ~0x7U;
constexpr unsigned long kMsixTableEntrySize = 16;
constexpr unsigned int kMsixVectorControlMaskedBit = 1U << 0;

constexpr unsigned short kVendorIdNoDevice = 0xFFFF;

constexpr unsigned short kCommandMemorySpaceBit = 1U << 1;
constexpr unsigned short kCommandBusMasterBit = 1U << 2;

// MMCONFIG(ECAM) 전용 가상주소 - LAPIC/IOAPIC/HPET 다음 슬롯들과
// 겹치지 않게 별도로 크게 떼어 뒀다(bus 0 하나치, 32개 장치 x 8
// 함수 x 4KiB = 1MiB).
constexpr unsigned long kMmconfigBus0VirtBase = 0xFFFF901000010000UL;
constexpr unsigned long kMmconfigPerFunctionSize = 4096;
constexpr unsigned long kMmconfigBus0Size = 32 * 8 * kMmconfigPerFunctionSize;  // 1MiB

bool gUseMmconfigBus0 = false;
unsigned long gMcfgBusOffset = 0;  // gMcfgBusOffset = bus0의 ECAM 물리 시작 주소(= McfgBase, startBus==0일 때)

unsigned int kLegacyConfigAddress(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset) {
    return kConfigAddressEnableBit | (static_cast<unsigned int>(bus) << 16) |
           (static_cast<unsigned int>(device & 0x1F) << 11) | (static_cast<unsigned int>(function & 0x07) << 8) |
           (offset & 0xFC);
}

// bus 0 + MMCONFIG 사용 가능 여부 - 이 조건일 때만 ECAM 경로를 쓰고,
// 그 외(다른 버스, 또는 MCFG 자체가 없음)는 전부 레거시로 폴백한다.
bool kShouldUseMmconfig(unsigned char bus) { return gUseMmconfigBus0 && bus == 0; }

unsigned long kMmconfigVirtAddress(unsigned char device, unsigned char function, unsigned char offset) {
    return kMmconfigBus0VirtBase + (static_cast<unsigned long>(device) * 8 + function) * kMmconfigPerFunctionSize +
           offset;
}

void kScanBus(unsigned char bus, kernel::Pci::EnumerateCallback callback);

void kScanFunction(unsigned char bus, unsigned char device, unsigned char function, kernel::Pci::EnumerateCallback callback) {
    const unsigned short vendorId = kernel::Pci::readConfig16(bus, device, function, kOffsetVendorId);
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
        const unsigned char secondaryBus = kernel::Pci::readConfig8(bus, device, function, kOffsetSecondaryBus);
        kScanBus(secondaryBus, callback);
    }
}

void kScanBus(unsigned char bus, kernel::Pci::EnumerateCallback callback) {
    for (unsigned int device = 0; device < 32; ++device) {
        const unsigned short vendorId = kernel::Pci::readConfig16(bus, static_cast<unsigned char>(device), 0, kOffsetVendorId);
        if (vendorId == kVendorIdNoDevice) {
            continue;
        }
        const unsigned char headerType = kernel::Pci::readConfig8(bus, static_cast<unsigned char>(device), 0, kOffsetHeaderType);
        const unsigned int functionCount = (headerType & kHeaderTypeMultiFunctionBit) ? 8 : 1;
        for (unsigned int function = 0; function < functionCount; ++function) {
            kScanFunction(bus, static_cast<unsigned char>(device), static_cast<unsigned char>(function), callback);
        }
    }
}

// BAR(offset 0x10 + 4*barIndex)의 물리 베이스 주소를 읽는다 - 64비트
// BAR(type bits01=10b)면 다음 슬롯과 합쳐 64비트 값을 만든다.
unsigned long kReadBarAddress(unsigned char bus, unsigned char device, unsigned char function, unsigned int barIndex) {
    const auto barOffset = static_cast<unsigned char>(kOffsetBar0 + barIndex * 4);
    const unsigned int low = kernel::Pci::readConfig32(bus, device, function, barOffset);
    constexpr unsigned int kBarTypeMask = 0x6;
    constexpr unsigned int kBarType64Bit = 0x4;
    unsigned long address = low & ~0xFUL;
    if ((low & kBarTypeMask) == kBarType64Bit) {
        const unsigned int high = kernel::Pci::readConfig32(bus, device, function, static_cast<unsigned char>(barOffset + 4));
        address |= static_cast<unsigned long>(high) << 32;
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
        return;  // bus 0이 이 세그먼트에 없음 - 레거시로만 동작(드문 구성, 관계도 기록)
    }
    gMcfgBusOffset = Acpi::mcfgBaseAddress();
    Paging::mapPage(kMmconfigBus0VirtBase, gMcfgBusOffset, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
    for (unsigned long off = kMmconfigPerFunctionSize; off < kMmconfigBus0Size; off += kMmconfigPerFunctionSize) {
        Paging::mapPage(kMmconfigBus0VirtBase + off, gMcfgBusOffset + off, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
    }
    gUseMmconfigBus0 = true;
}

bool Pci::usesMmconfig() { return gUseMmconfigBus0; }

unsigned int Pci::readConfig32(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset) {
    if (kShouldUseMmconfig(bus)) {
        return *reinterpret_cast<volatile unsigned int*>(kMmconfigVirtAddress(device, function, offset));
    }
    arch::kOutL(kConfigAddressPort, kLegacyConfigAddress(bus, device, function, offset));
    return arch::kInL(kConfigDataPort);
}

void Pci::writeConfig32(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset, unsigned int value) {
    if (kShouldUseMmconfig(bus)) {
        *reinterpret_cast<volatile unsigned int*>(kMmconfigVirtAddress(device, function, offset)) = value;
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
unsigned short Pci::readConfig16(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset) {
    if (kShouldUseMmconfig(bus)) {
        return *reinterpret_cast<volatile unsigned short*>(kMmconfigVirtAddress(device, function, offset));
    }
    arch::kOutL(kConfigAddressPort, kLegacyConfigAddress(bus, device, function, offset));
    return arch::kInW(static_cast<unsigned short>(kConfigDataPort + (offset & 2)));
}

void Pci::writeConfig16(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset, unsigned short value) {
    if (kShouldUseMmconfig(bus)) {
        *reinterpret_cast<volatile unsigned short*>(kMmconfigVirtAddress(device, function, offset)) = value;
        return;
    }
    arch::kOutL(kConfigAddressPort, kLegacyConfigAddress(bus, device, function, offset));
    arch::kOutW(static_cast<unsigned short>(kConfigDataPort + (offset & 2)), value);
}

unsigned char Pci::readConfig8(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset) {
    if (kShouldUseMmconfig(bus)) {
        return *reinterpret_cast<volatile unsigned char*>(kMmconfigVirtAddress(device, function, offset));
    }
    arch::kOutL(kConfigAddressPort, kLegacyConfigAddress(bus, device, function, offset));
    return arch::kInB(static_cast<unsigned short>(kConfigDataPort + (offset & 3)));
}

void Pci::enumerate(EnumerateCallback callback) {
    kScanBus(0, callback);
}

unsigned char Pci::findCapability(unsigned char bus, unsigned char device, unsigned char function, unsigned char capabilityId) {
    const unsigned short status = readConfig16(bus, device, function, kOffsetStatus);
    if (!(status & kStatusCapabilitiesListBit)) {
        return 0;
    }

    unsigned char ptr = readConfig8(bus, device, function, kOffsetCapabilitiesPointer) & 0xFC;
    while (ptr != 0) {
        const unsigned char id = readConfig8(bus, device, function, ptr);
        if (id == capabilityId) {
            return ptr;
        }
        ptr = readConfig8(bus, device, function, static_cast<unsigned char>(ptr + 1)) & 0xFC;
    }
    return 0;
}

bool Pci::enableMsi(unsigned char bus, unsigned char device, unsigned char function, unsigned int vector, unsigned int destApicId) {
    unsigned int grantedBase = 0;
    unsigned int grantedCount = 0;
    if (!enableMsiVectors(bus, device, function, 1, vector, destApicId, &grantedBase, &grantedCount)) {
        return false;
    }
    return grantedCount >= 1;
}

bool Pci::enableMsiVectors(unsigned char bus, unsigned char device, unsigned char function, unsigned int requestedCount,
                            unsigned int preferredBase, unsigned int destApicId, unsigned int* outBaseVector,
                            unsigned int* outGrantedCount) {
    *outBaseVector = 0;
    *outGrantedCount = 0;

    const unsigned char msiOffset = findCapability(bus, device, function, kCapabilityIdMsi);
    if (!msiOffset) {
        return false;
    }
    if (destApicId > 0xFF) {
        // 클래식 MSI Message Address의 물리 목적지 필드는 IOAPIC
        // REDTBL과 마찬가지로 8비트 고정이다(SDM Vol.3 10.11.1) -
        // 조용히 자르지 않고 실패를 알린다(ioapic.cpp와 같은 원칙).
        return false;
    }

    unsigned short messageControl = readConfig16(bus, device, function, static_cast<unsigned char>(msiOffset + 2));
    const bool is64BitCapable = (messageControl & kMsiControl64BitCapableBit) != 0;
    const unsigned int multiMessageCapableLog2 =
        (messageControl & kMsiControlMultiMessageCapableMask) >> kMsiControlMultiMessageCapableShift;
    const unsigned int deviceMaxVectors = 1U << multiMessageCapableLog2;

    // requestedCount를 2의 거듭제곱으로 내림하고 장치 한도로 다시
    // 제한한다(MSI 하드웨어 자체가 2의 거듭제곱 개수만 지원).
    unsigned int grantedCount = 1;
    while (grantedCount * 2 <= requestedCount && grantedCount * 2 <= deviceMaxVectors) {
        grantedCount *= 2;
    }
    // preferredBase를 grantedCount의 배수로 내림(MSI는 벡터 그룹이
    // 그 개수만큼 정렬돼 있어야 한다 - 하드웨어가 데이터 레지스터의
    // 하위 로그2(count)비트를 인터럽트 인덱스로 자동 채우는 방식이라).
    const unsigned int baseVector = preferredBase & ~(grantedCount - 1);

    const unsigned int messageAddress = 0xFEE00000U | ((destApicId & 0xFF) << 12);
    const auto messageData = static_cast<unsigned short>(baseVector & 0xFF);

    writeConfig32(bus, device, function, static_cast<unsigned char>(msiOffset + 4), messageAddress);
    if (is64BitCapable) {
        writeConfig32(bus, device, function, static_cast<unsigned char>(msiOffset + 8), 0);  // 상위 32비트 - LAPIC은 항상 4GiB 이하
        writeConfig16(bus, device, function, static_cast<unsigned char>(msiOffset + 12), messageData);
    } else {
        writeConfig16(bus, device, function, static_cast<unsigned char>(msiOffset + 8), messageData);
    }

    unsigned int mmeLog2 = 0;
    while ((1U << mmeLog2) < grantedCount) {
        ++mmeLog2;
    }
    messageControl = static_cast<unsigned short>(messageControl & ~kMsiControlMultiMessageEnableMask);
    messageControl = static_cast<unsigned short>(messageControl | (mmeLog2 << kMsiControlMultiMessageEnableShift));
    messageControl = static_cast<unsigned short>(messageControl | kMsiControlEnableBit);
    writeConfig16(bus, device, function, static_cast<unsigned char>(msiOffset + 2), messageControl);

    *outBaseVector = baseVector;
    *outGrantedCount = grantedCount;
    return true;
}

bool Pci::enableMsix(unsigned char bus, unsigned char device, unsigned char function, unsigned int tableIndex,
                      unsigned int vector, unsigned int destApicId) {
    const unsigned char msixOffset = findCapability(bus, device, function, kCapabilityIdMsix);
    if (!msixOffset) {
        return false;
    }

    unsigned short messageControl = readConfig16(bus, device, function, static_cast<unsigned char>(msixOffset + 2));
    const unsigned int tableSize = (messageControl & kMsixControlTableSizeMask) + 1;
    if (tableIndex >= tableSize) {
        return false;
    }

    const unsigned int tableOffsetBir = readConfig32(bus, device, function, static_cast<unsigned char>(msixOffset + 4));
    const unsigned int bir = tableOffsetBir & kMsixBirMask;
    const unsigned int tableOffsetInBar = tableOffsetBir & kMsixOffsetMask;

    const unsigned long barPhys = kReadBarAddress(bus, device, function, bir);
    const unsigned long tablePhys = barPhys + tableOffsetInBar;
    const unsigned long entryPhys = tablePhys + tableIndex * kMsixTableEntrySize;

    // 이 엔트리가 속한 4KiB 페이지 하나만 매핑한다 - 여러 장치/여러
    // 호출이 겹칠 수 있어 매번 같은 임시 가상주소를 재사용(직후에
    // 바로 다 써버리므로 안전 - 영구 매핑이 필요해지면 그때 캐시).
    constexpr unsigned long kMsixTableVirtBase = 0xFFFF901000110000UL;
    const unsigned long pageBase = entryPhys & ~0xFFFUL;
    const unsigned long pageOffset = entryPhys & 0xFFFUL;
    Paging::mapPage(kMsixTableVirtBase, pageBase, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
    auto* entry = reinterpret_cast<volatile unsigned int*>(kMsixTableVirtBase + pageOffset);

    const unsigned int messageAddress = 0xFEE00000U | ((destApicId & 0xFF) << 12);
    entry[0] = messageAddress;  // Message Address Low
    entry[1] = 0;               // Message Address High - LAPIC은 항상 4GiB 이하
    entry[2] = vector & 0xFF;   // Message Data
    entry[3] = entry[3] & ~kMsixVectorControlMaskedBit;  // 이 엔트리 마스크 해제

    const unsigned short command = readConfig16(bus, device, function, kOffsetCommand);
    writeConfig16(bus, device, function, kOffsetCommand,
                  static_cast<unsigned short>(command | kCommandMemorySpaceBit | kCommandBusMasterBit));

    messageControl = static_cast<unsigned short>(messageControl & ~kMsixControlFunctionMaskBit);
    messageControl = static_cast<unsigned short>(messageControl | kMsixControlEnableBit);
    writeConfig16(bus, device, function, static_cast<unsigned char>(msixOffset + 2), messageControl);
    return true;
}

}  // namespace kernel
