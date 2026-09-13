#include "pci.h"

#include "x86_64/io_port.h"

namespace {

constexpr unsigned short kConfigAddressPort = 0xCF8;
constexpr unsigned short kConfigDataPort = 0xCFC;
constexpr unsigned int kConfigAddressEnableBit = 1U << 31;

constexpr unsigned char kOffsetVendorId = 0x00;
constexpr unsigned char kOffsetDeviceId = 0x02;
constexpr unsigned char kOffsetStatus = 0x06;
constexpr unsigned char kOffsetRevisionId = 0x08;
constexpr unsigned char kOffsetProgIf = 0x09;
constexpr unsigned char kOffsetSubclass = 0x0A;
constexpr unsigned char kOffsetClassCode = 0x0B;
constexpr unsigned char kOffsetHeaderType = 0x0E;
constexpr unsigned char kOffsetSecondaryBus = 0x19;
constexpr unsigned char kOffsetCapabilitiesPointer = 0x34;

constexpr unsigned short kStatusCapabilitiesListBit = 1U << 4;
constexpr unsigned char kHeaderTypeMultiFunctionBit = 0x80;
constexpr unsigned char kHeaderTypeMask = 0x7F;

constexpr unsigned char kClassBridge = 0x06;
constexpr unsigned char kSubclassPciToPciBridge = 0x04;

constexpr unsigned char kCapabilityIdMsi = 0x05;

constexpr unsigned short kMsiControlEnableBit = 1U << 0;
constexpr unsigned short kMsiControl64BitCapableBit = 1U << 7;
constexpr unsigned short kMsiControlMultiMessageEnableMask = 0x7U << 4;

constexpr unsigned short kVendorIdNoDevice = 0xFFFF;

unsigned int kConfigAddress(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset) {
    return kConfigAddressEnableBit | (static_cast<unsigned int>(bus) << 16) |
           (static_cast<unsigned int>(device & 0x1F) << 11) | (static_cast<unsigned int>(function & 0x07) << 8) |
           (offset & 0xFC);
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

}  // namespace

namespace kernel {

unsigned int Pci::readConfig32(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset) {
    arch::kOutL(kConfigAddressPort, kConfigAddress(bus, device, function, offset));
    return arch::kInL(kConfigDataPort);
}

void Pci::writeConfig32(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset, unsigned int value) {
    arch::kOutL(kConfigAddressPort, kConfigAddress(bus, device, function, offset));
    arch::kOutL(kConfigDataPort, value);
}

// CONFIG_DATA(0xCFC)에 오프셋의 하위 2비트를 더한 포트로 접근하면
// 칩셋이 해당 폭(8/16/32비트)의 바이트 레인만 골라준다 - 32비트로
// 통째로 읽어 마스킹/시프트하는 것보다 실제 하드웨어 동작과 일치하고
// 더 간단하다.
unsigned short Pci::readConfig16(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset) {
    arch::kOutL(kConfigAddressPort, kConfigAddress(bus, device, function, offset));
    return arch::kInW(static_cast<unsigned short>(kConfigDataPort + (offset & 2)));
}

void Pci::writeConfig16(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset, unsigned short value) {
    arch::kOutL(kConfigAddressPort, kConfigAddress(bus, device, function, offset));
    arch::kOutW(static_cast<unsigned short>(kConfigDataPort + (offset & 2)), value);
}

unsigned char Pci::readConfig8(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset) {
    arch::kOutL(kConfigAddressPort, kConfigAddress(bus, device, function, offset));
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
    const unsigned char msiOffset = findCapability(bus, device, function, kCapabilityIdMsi);
    if (!msiOffset) {
        return false;
    }

    unsigned short messageControl = readConfig16(bus, device, function, static_cast<unsigned char>(msiOffset + 2));
    const bool is64BitCapable = (messageControl & kMsiControl64BitCapableBit) != 0;

    // Message Address(LAPIC MMIO 규약, 델리버리모드=Fixed/목적지모드
    // =physical 기본값) + Message Data(벡터 번호만 - 델리버리모드/
    // 트리거모드 비트는 전부 0=Fixed/edge 기본값).
    const unsigned int messageAddress = 0xFEE00000U | ((destApicId & 0xFF) << 12);
    const auto messageData = static_cast<unsigned short>(vector & 0xFF);

    writeConfig32(bus, device, function, static_cast<unsigned char>(msiOffset + 4), messageAddress);
    if (is64BitCapable) {
        writeConfig32(bus, device, function, static_cast<unsigned char>(msiOffset + 8), 0);  // 상위 32비트 - LAPIC은 항상 4GiB 이하
        writeConfig16(bus, device, function, static_cast<unsigned char>(msiOffset + 12), messageData);
    } else {
        writeConfig16(bus, device, function, static_cast<unsigned char>(msiOffset + 8), messageData);
    }

    messageControl = static_cast<unsigned short>((messageControl & ~kMsiControlMultiMessageEnableMask) | kMsiControlEnableBit);
    writeConfig16(bus, device, function, static_cast<unsigned char>(msiOffset + 2), messageControl);
    return true;
}

}  // namespace kernel
