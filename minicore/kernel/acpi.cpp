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

struct MadtInterruptSourceOverrideEntry {
    MadtEntryHeader header;
    unsigned char bus;   // 항상 0(ISA)
    unsigned char source;  // ISA IRQ 번호
    unsigned int globalSystemInterrupt;
    unsigned short flags;  // MPS INTI Flags: bit0-1 극성, bit2-3 트리거 모드
} __attribute__((packed));

constexpr unsigned char kMadtTypeLocalApic = 0;
constexpr unsigned char kMadtTypeIoApic = 1;
constexpr unsigned char kMadtTypeInterruptSourceOverride = 2;
constexpr unsigned int kMadtLocalApicEnabledFlag = 1U << 0;

// MPS INTI Flags(ACPI 스펙 5.2.12.5) - 극성 2비트/트리거 2비트, 각각
// 0=버스 기본값(conforms), 1=고정값, 2=예약, 3=반대 고정값.
constexpr unsigned short kMpsFlagsPolarityMask = 0x3;
constexpr unsigned short kMpsFlagsTriggerShift = 2;
constexpr unsigned short kMpsFlagsTriggerMask = 0x3 << kMpsFlagsTriggerShift;

struct SratEntryHeader {
    unsigned char type;
    unsigned char length;
} __attribute__((packed));

struct SratProcessorApicAffinity {
    SratEntryHeader header;
    unsigned char proximityDomainLow;
    unsigned char apicId;
    unsigned int flags;
    unsigned char localSapicEid;
    unsigned char proximityDomainHigh[3];
    unsigned int clockDomain;
} __attribute__((packed));

struct SratMemoryAffinity {
    SratEntryHeader header;
    unsigned int proximityDomain;
    unsigned short reserved1;
    unsigned int baseAddressLow;
    unsigned int baseAddressHigh;
    unsigned int lengthLow;
    unsigned int lengthHigh;
    unsigned int reserved2;
    unsigned int flags;
    unsigned long reserved3;
} __attribute__((packed));

constexpr unsigned char kSratTypeProcessorApicAffinity = 0;
constexpr unsigned char kSratTypeMemoryAffinity = 1;
constexpr unsigned int kSratAffinityEnabledFlag = 1U << 0;
constexpr unsigned int kSratHeaderPad = 12;  // tableRevision(4) + reserved(8)

// ACPI HPET 테이블(IA-PC HPET 규격 3.2.4) - SdtHeader(36바이트) 바로
// 뒤에 이어진다. Generic Address Structure(주소공간ID+너비+비트오프셋
// +예약+실제주소, 12바이트)에서 실제 주소만 쓴다(항상 시스템 메모리
// 공간이라 addressSpaceId 검사는 생략 - 다른 값이 실무에서 쓰이는
// 사례가 없음).
struct HpetTable {
    SdtHeader header;
    unsigned int eventTimerBlockId;
    unsigned char addressSpaceId;
    unsigned char registerBitWidth;
    unsigned char registerBitOffset;
    unsigned char reserved0;
    unsigned long address;
    unsigned char hpetNumber;
    unsigned short minimumTick;
    unsigned char pageProtection;
} __attribute__((packed));

// ACPI MCFG(PCI Firmware Spec 3.0 §4.1.2) - SdtHeader(36바이트) +
// 예약 8바이트 뒤에 가변 개수의 엔트리가 이어진다. 세그먼트 그룹 0
// (첫 엔트리)만 다룬다.
struct McfgEntry {
    unsigned long baseAddress;
    unsigned short pciSegmentGroup;
    unsigned char startBusNumber;
    unsigned char endBusNumber;
    unsigned int reserved;
} __attribute__((packed));
constexpr unsigned int kMcfgHeaderPad = 8;  // 예약 필드

unsigned long gLocalApicAddress = 0;
unsigned int gIoApicAddress = 0;
bool gHasHpet = false;
unsigned long gHpetAddress = 0;
bool gHasMcfg = false;
unsigned long gMcfgBaseAddress = 0;
unsigned char gMcfgStartBus = 0;
unsigned char gMcfgEndBus = 0;
unsigned int gCpuApicIds[kernel::kAcpiMaxCpus];
unsigned int gCpuNumaNode[kernel::kAcpiMaxCpus];
unsigned int gCpuCount = 0;

// ISA IRQ(인덱스) -> override 존재 여부/GSI/극성/트리거. override가
// 없는 IRQ는 gIsoPresent[irq]==false로 남고, Acpi::resolveIsaIrq가
// ISA 기본값(GSI=IRQ, active-high, edge)으로 채워 돌려준다.
bool gIsoPresent[kernel::kAcpiMaxIsoEntries];
unsigned int gIsoGsi[kernel::kAcpiMaxIsoEntries];
unsigned int gIsoPolarity[kernel::kAcpiMaxIsoEntries];
unsigned int gIsoTriggerMode[kernel::kAcpiMaxIsoEntries];

unsigned int gDomainValues[kernel::kAcpiMaxNumaNodes];
unsigned int gDomainCount = 0;

struct MemAffinity {
    unsigned long base;
    unsigned long length;
    unsigned int node;
};
MemAffinity gMemAffinities[kernel::kAcpiMaxMemoryAffinityEntries];
unsigned int gMemAffinityCount = 0;

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

// proximity domain 값(펌웨어가 매긴 임의의 ID)을 0부터 시작하는
// 내부 노드 인덱스로 바꾼다 - 처음 보는 값이면 새 노드를 만든다.
unsigned int kNodeIndexForDomain(unsigned int domain) {
    for (unsigned int i = 0; i < gDomainCount; ++i) {
        if (gDomainValues[i] == domain) {
            return i;
        }
    }
    if (gDomainCount < kernel::kAcpiMaxNumaNodes) {
        gDomainValues[gDomainCount] = domain;
        return gDomainCount++;
    }
    return 0;  // 상한 초과 - 방어적으로 노드0에 몰아넣는다
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
                gCpuApicIds[gCpuCount] = lapic->apicId;
                gCpuNumaNode[gCpuCount] = 0;  // SRAT가 있으면 kParseSrat이 덮어씀
                ++gCpuCount;
            }
        } else if (entryHeader->type == kMadtTypeIoApic) {
            const auto* ioapic = reinterpret_cast<const MadtIoApicEntry*>(entry);
            gIoApicAddress = ioapic->ioApicAddress;
        } else if (entryHeader->type == kMadtTypeInterruptSourceOverride) {
            const auto* iso = reinterpret_cast<const MadtInterruptSourceOverrideEntry*>(entry);
            if (iso->source < kernel::kAcpiMaxIsoEntries) {
                gIsoPresent[iso->source] = true;
                gIsoGsi[iso->source] = iso->globalSystemInterrupt;

                const unsigned int rawPolarity = iso->flags & kMpsFlagsPolarityMask;
                gIsoPolarity[iso->source] =
                    (rawPolarity == 0 || rawPolarity == kernel::kAcpiPolarityActiveHigh)
                        ? kernel::kAcpiPolarityActiveHigh
                        : kernel::kAcpiPolarityActiveLow;

                const unsigned int rawTrigger = (iso->flags & kMpsFlagsTriggerMask) >> kMpsFlagsTriggerShift;
                gIsoTriggerMode[iso->source] =
                    (rawTrigger == 0 || rawTrigger == kernel::kAcpiTriggerEdge)
                        ? kernel::kAcpiTriggerEdge
                        : kernel::kAcpiTriggerLevel;
            }
        }
        entry += entryHeader->length;
    }
}

void kParseSrat(const SdtHeader* sratHeader) {
    const auto* base = reinterpret_cast<const unsigned char*>(sratHeader);
    const unsigned char* entry = base + sizeof(SdtHeader) + kSratHeaderPad;
    const unsigned char* end = base + sratHeader->length;

    while (entry < end) {
        const auto* entryHeader = reinterpret_cast<const SratEntryHeader*>(entry);
        if (entryHeader->length == 0) {
            break;
        }
        if (entryHeader->type == kSratTypeProcessorApicAffinity) {
            const auto* p = reinterpret_cast<const SratProcessorApicAffinity*>(entry);
            if (p->flags & kSratAffinityEnabledFlag) {
                const unsigned int domain = p->proximityDomainLow |
                                             (static_cast<unsigned int>(p->proximityDomainHigh[0]) << 8) |
                                             (static_cast<unsigned int>(p->proximityDomainHigh[1]) << 16) |
                                             (static_cast<unsigned int>(p->proximityDomainHigh[2]) << 24);
                const unsigned int node = kNodeIndexForDomain(domain);
                for (unsigned int i = 0; i < gCpuCount; ++i) {
                    if (gCpuApicIds[i] == p->apicId) {
                        gCpuNumaNode[i] = node;
                        break;
                    }
                }
            }
        } else if (entryHeader->type == kSratTypeMemoryAffinity) {
            const auto* m = reinterpret_cast<const SratMemoryAffinity*>(entry);
            if ((m->flags & kSratAffinityEnabledFlag) && gMemAffinityCount < kernel::kAcpiMaxMemoryAffinityEntries) {
                const unsigned long memBase = (static_cast<unsigned long>(m->baseAddressHigh) << 32) | m->baseAddressLow;
                const unsigned long memLength = (static_cast<unsigned long>(m->lengthHigh) << 32) | m->lengthLow;
                gMemAffinities[gMemAffinityCount++] = {memBase, memLength, kNodeIndexForDomain(m->proximityDomain)};
            }
        }
        entry += entryHeader->length;
    }
}

// RSDT(32비트 포인터 배열) 또는 XSDT(64비트 포인터 배열)를 훑어서
// signature와 일치하는 테이블을 찾는다 - 둘 다 헤더 형태가 같아서
// 템플릿 하나로 처리한다.
template <typename PointerType>
const SdtHeader* kFindTable(unsigned long sdtPhys, const char* signature) {
    const auto* header = kAsTable<SdtHeader>(sdtPhys);
    if (!kChecksumOk(header, header->length)) {
        return nullptr;
    }
    const auto* base = reinterpret_cast<const unsigned char*>(header);
    const auto* pointers = reinterpret_cast<const PointerType*>(base + sizeof(SdtHeader));
    const unsigned int count = (header->length - sizeof(SdtHeader)) / sizeof(PointerType);

    for (unsigned int i = 0; i < count; ++i) {
        const auto* candidate = kAsTable<SdtHeader>(static_cast<unsigned long>(pointers[i]));
        if (kSignatureIs(candidate->signature, signature, 4) && kChecksumOk(candidate, candidate->length)) {
            return candidate;
        }
    }
    return nullptr;
}

}  // namespace

namespace kernel {

bool Acpi::init(unsigned long rsdpPhys) {
    const auto* rsdp = kAsTable<Rsdp>(rsdpPhys);
    if (!kSignatureIs(rsdp->signature, "RSD PTR ", 8)) {
        return false;
    }

    const SdtHeader* madt = nullptr;
    const SdtHeader* srat = nullptr;
    const SdtHeader* hpet = nullptr;
    const SdtHeader* mcfg = nullptr;

    if (rsdp->revision >= 2 && kChecksumOk(rsdp, sizeof(Rsdp)) && rsdp->xsdtAddress) {
        madt = kFindTable<unsigned long>(rsdp->xsdtAddress, "APIC");
        srat = kFindTable<unsigned long>(rsdp->xsdtAddress, "SRAT");
        hpet = kFindTable<unsigned long>(rsdp->xsdtAddress, "HPET");
        mcfg = kFindTable<unsigned long>(rsdp->xsdtAddress, "MCFG");
    } else if (kChecksumOk(rsdp, 20)) {  // ACPI 1.0 RSDP는 처음 20바이트만 체크섬 대상
        madt = kFindTable<unsigned int>(rsdp->rsdtAddress, "APIC");
        srat = kFindTable<unsigned int>(rsdp->rsdtAddress, "SRAT");
        hpet = kFindTable<unsigned int>(rsdp->rsdtAddress, "HPET");
        mcfg = kFindTable<unsigned int>(rsdp->rsdtAddress, "MCFG");
    } else {
        return false;
    }

    if (hpet) {
        gHasHpet = true;
        gHpetAddress = reinterpret_cast<const HpetTable*>(hpet)->address;
    }

    if (mcfg && mcfg->length >= sizeof(SdtHeader) + kMcfgHeaderPad + sizeof(McfgEntry)) {
        const auto* mcfgBase = reinterpret_cast<const unsigned char*>(mcfg);
        const auto* entry = reinterpret_cast<const McfgEntry*>(mcfgBase + sizeof(SdtHeader) + kMcfgHeaderPad);
        gHasMcfg = true;
        gMcfgBaseAddress = entry->baseAddress;
        gMcfgStartBus = entry->startBusNumber;
        gMcfgEndBus = entry->endBusNumber;
    }

    if (!madt) {
        return false;
    }
    kParseMadt(madt);

    if (srat) {
        kParseSrat(srat);
    } else {
        // SRAT가 없는 머신(전형적으로 비-NUMA, 예: QEMU 기본 설정) -
        // 노드 하나로 취급한다. memoryAffinityCount()==0으로 남으므로
        // 호출부(PageFrameAllocator)가 "정보 없음 = 전부 노드 0"으로
        // 처리해야 한다.
        gDomainCount = 1;
        gDomainValues[0] = 0;
        for (unsigned int i = 0; i < gCpuCount; ++i) {
            gCpuNumaNode[i] = 0;
        }
    }
    return true;
}

unsigned long Acpi::localApicAddress() { return gLocalApicAddress; }
unsigned int Acpi::cpuCount() { return gCpuCount; }
unsigned int Acpi::cpuApicId(unsigned int index) { return gCpuApicIds[index]; }
unsigned int Acpi::ioApicAddress() { return gIoApicAddress; }

Acpi::IsaIrqRouting Acpi::resolveIsaIrq(unsigned int isaIrq) {
    if (isaIrq < kAcpiMaxIsoEntries && gIsoPresent[isaIrq]) {
        return {gIsoGsi[isaIrq], gIsoPolarity[isaIrq], gIsoTriggerMode[isaIrq]};
    }
    // override 없음 - ACPI 스펙의 ISA 기본값(GSI=IRQ, active-high, edge).
    return {isaIrq, kAcpiPolarityActiveHigh, kAcpiTriggerEdge};
}

bool Acpi::hasHpet() { return gHasHpet; }
unsigned long Acpi::hpetAddress() { return gHpetAddress; }

bool Acpi::hasMcfg() { return gHasMcfg; }
unsigned long Acpi::mcfgBaseAddress() { return gMcfgBaseAddress; }
unsigned char Acpi::mcfgStartBus() { return gMcfgStartBus; }
unsigned char Acpi::mcfgEndBus() { return gMcfgEndBus; }

unsigned int Acpi::numaNodeCount() { return gDomainCount; }
unsigned int Acpi::cpuNumaNode(unsigned int index) { return gCpuNumaNode[index]; }

unsigned int Acpi::memoryAffinityCount() { return gMemAffinityCount; }
unsigned long Acpi::memoryAffinityBase(unsigned int index) { return gMemAffinities[index].base; }
unsigned long Acpi::memoryAffinityLength(unsigned int index) { return gMemAffinities[index].length; }
unsigned int Acpi::memoryAffinityNode(unsigned int index) { return gMemAffinities[index].node; }

}  // namespace kernel
