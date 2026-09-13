#include "acpi.h"

#include "paging.h"

namespace {

struct Rsdp {
    char signature[8];
    unsigned char checksum;
    char oemId[6];
    unsigned char revision;
    unsigned int rsdtAddress;
    // ACPI 2.0+에만 유효(revision >= 2) - 그 이하 리비전에서는 이
    // 뒤를 읽으면 안 된다(구조체 길이 자체가 짧음).
    unsigned int length;
    unsigned long xsdtAddress;
    unsigned char extendedChecksum;
    unsigned char reserved[3];
} __attribute__((packed));

struct SdtHeader {
    char signature[4];
    unsigned int length;
    unsigned char revision;
    unsigned char checksum;
    char oemId[6];
    char oemTableId[8];
    unsigned int oemRevision;
    unsigned int creatorId;
    unsigned int creatorRevision;
} __attribute__((packed));

struct MadtEntryHeader {
    unsigned char type;
    unsigned char length;
} __attribute__((packed));

struct MadtLocalApicEntry {
    MadtEntryHeader header;
    unsigned char acpiProcessorId;
    unsigned char apicId;
    unsigned int flags;
} __attribute__((packed));

struct MadtIoApicEntry {
    MadtEntryHeader header;
    unsigned char ioApicId;
    unsigned char reserved;
    unsigned int ioApicAddress;
    unsigned int globalSystemInterruptBase;
} __attribute__((packed));

constexpr unsigned char kMadtTypeLocalApic = 0;
constexpr unsigned char kMadtTypeIoApic = 1;
constexpr unsigned int kMadtLocalApicEnabledFlag = 1U << 0;

unsigned long gLocalApicAddress = 0;
unsigned int gIoApicAddress = 0;
unsigned int gCpuApicIds[kernel::kAcpiMaxCpus];
unsigned int gCpuCount = 0;

template <typename T>
const T* kAsTable(unsigned long physAddr) {
    return reinterpret_cast<const T*>(kernel::kPhysToVirt(physAddr));
}

bool kChecksumOk(const void* data, unsigned int length) {
    unsigned char sum = 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    for (unsigned int i = 0; i < length; ++i) {
        sum = static_cast<unsigned char>(sum + bytes[i]);
    }
    return sum == 0;
}

bool kSignatureIs(const char* sig, const char* expected, int len) {
    for (int i = 0; i < len; ++i) {
        if (sig[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

void kParseMadt(const SdtHeader* madtHeader) {
    const auto* base = reinterpret_cast<const unsigned char*>(madtHeader);
    const auto* localApicAddrField = reinterpret_cast<const unsigned int*>(base + sizeof(SdtHeader));
    gLocalApicAddress = *localApicAddrField;

    const unsigned char* entry = base + sizeof(SdtHeader) + 8;  // localApicAddress(4) + flags(4)
    const unsigned char* end = base + madtHeader->length;

    while (entry < end) {
        const auto* entryHeader = reinterpret_cast<const MadtEntryHeader*>(entry);
        if (entryHeader->length == 0) {
            break;  // 손상 방어 - 무한루프 방지
        }
        if (entryHeader->type == kMadtTypeLocalApic) {
            const auto* lapic = reinterpret_cast<const MadtLocalApicEntry*>(entry);
            if ((lapic->flags & kMadtLocalApicEnabledFlag) && gCpuCount < kernel::kAcpiMaxCpus) {
                gCpuApicIds[gCpuCount++] = lapic->apicId;
            }
        } else if (entryHeader->type == kMadtTypeIoApic) {
            const auto* ioapic = reinterpret_cast<const MadtIoApicEntry*>(entry);
            gIoApicAddress = ioapic->ioApicAddress;
        }
        entry += entryHeader->length;
    }
}

// RSDT(32비트 포인터 배열) 또는 XSDT(64비트 포인터 배열)를 훑어서
// "APIC"(MADT) 테이블을 찾는다 - 둘 다 헤더 형태가 같아서 템플릿
// 하나로 처리한다.
template <typename PointerType>
bool kFindAndParseMadt(unsigned long sdtPhys) {
    const auto* header = kAsTable<SdtHeader>(sdtPhys);
    if (!kChecksumOk(header, header->length)) {
        return false;
    }
    const auto* base = reinterpret_cast<const unsigned char*>(header);
    const auto* pointers = reinterpret_cast<const PointerType*>(base + sizeof(SdtHeader));
    const unsigned int count = (header->length - sizeof(SdtHeader)) / sizeof(PointerType);

    for (unsigned int i = 0; i < count; ++i) {
        const auto* candidate = kAsTable<SdtHeader>(static_cast<unsigned long>(pointers[i]));
        if (kSignatureIs(candidate->signature, "APIC", 4) && kChecksumOk(candidate, candidate->length)) {
            kParseMadt(candidate);
            return true;
        }
    }
    return false;
}

}  // namespace

namespace kernel {

bool Acpi::init(unsigned long rsdpPhys) {
    const auto* rsdp = kAsTable<Rsdp>(rsdpPhys);
    if (!kSignatureIs(rsdp->signature, "RSD PTR ", 8)) {
        return false;
    }

    if (rsdp->revision >= 2 && kChecksumOk(rsdp, sizeof(Rsdp)) && rsdp->xsdtAddress) {
        return kFindAndParseMadt<unsigned long>(rsdp->xsdtAddress);
    }
    if (kChecksumOk(rsdp, 20)) {  // ACPI 1.0 RSDP는 처음 20바이트만 체크섬 대상
        return kFindAndParseMadt<unsigned int>(rsdp->rsdtAddress);
    }
    return false;
}

unsigned long Acpi::localApicAddress() { return gLocalApicAddress; }
unsigned int Acpi::cpuCount() { return gCpuCount; }
unsigned int Acpi::cpuApicId(unsigned int index) { return gCpuApicIds[index]; }
unsigned int Acpi::ioApicAddress() { return gIoApicAddress; }

}  // namespace kernel
