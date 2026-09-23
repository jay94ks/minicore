#include "acpi.h"

#include "libkenv/types.h"
#include "paging.h"

namespace {

struct Rsdp {
    char signature[8];
    kernel::uint8_t checksum;
    char oemId[6];
    kernel::uint8_t revision;
    kernel::uint32_t rsdtAddress;
    // ACPI 2.0+에만 유효(revision >= 2) - 그 이하 리비전에서는 이
    // 뒤를 읽으면 안 된다(구조체 길이 자체가 짧음).
    kernel::uint32_t length;
    kernel::uint64_t xsdtAddress;
    kernel::uint8_t extendedChecksum;
    kernel::uint8_t reserved[3];
} __attribute__((packed));

struct SdtHeader {
    char signature[4];
    kernel::uint32_t length;
    kernel::uint8_t revision;
    kernel::uint8_t checksum;
    char oemId[6];
    char oemTableId[8];
    kernel::uint32_t oemRevision;
    kernel::uint32_t creatorId;
    kernel::uint32_t creatorRevision;
} __attribute__((packed));

struct MadtEntryHeader {
    kernel::uint8_t type;
    kernel::uint8_t length;
} __attribute__((packed));

struct MadtLocalApicEntry {
    MadtEntryHeader header;
    kernel::uint8_t acpiProcessorId;
    kernel::uint8_t apicId;
    kernel::uint32_t flags;
} __attribute__((packed));

struct MadtIoApicEntry {
    MadtEntryHeader header;
    kernel::uint8_t ioApicId;
    kernel::uint8_t reserved;
    kernel::uint32_t ioApicAddress;
    kernel::uint32_t globalSystemInterruptBase;
} __attribute__((packed));

struct MadtInterruptSourceOverrideEntry {
    MadtEntryHeader header;
    kernel::uint8_t bus;   // 항상 0(ISA)
    kernel::uint8_t source;  // ISA IRQ 번호
    kernel::uint32_t globalSystemInterrupt;
    kernel::uint16_t flags;  // MPS INTI Flags: bit0-1 극성, bit2-3 트리거 모드
} __attribute__((packed));

constexpr kernel::uint8_t kMadtTypeLocalApic = 0;
constexpr kernel::uint8_t kMadtTypeIoApic = 1;
constexpr kernel::uint8_t kMadtTypeInterruptSourceOverride = 2;
constexpr kernel::uint32_t kMadtLocalApicEnabledFlag = 1U << 0;

// MPS INTI Flags(ACPI 스펙 5.2.12.5) - 극성 2비트/트리거 2비트, 각각
// 0=버스 기본값(conforms), 1=고정값, 2=예약, 3=반대 고정값.
constexpr kernel::uint16_t kMpsFlagsPolarityMask = 0x3;
constexpr kernel::uint16_t kMpsFlagsTriggerShift = 2;
constexpr kernel::uint16_t kMpsFlagsTriggerMask = 0x3 << kMpsFlagsTriggerShift;

struct SratEntryHeader {
    kernel::uint8_t type;
    kernel::uint8_t length;
} __attribute__((packed));

struct SratProcessorApicAffinity {
    SratEntryHeader header;
    kernel::uint8_t proximityDomainLow;
    kernel::uint8_t apicId;
    kernel::uint32_t flags;
    kernel::uint8_t localSapicEid;
    kernel::uint8_t proximityDomainHigh[3];
    kernel::uint32_t clockDomain;
} __attribute__((packed));

struct SratMemoryAffinity {
    SratEntryHeader header;
    kernel::uint32_t proximityDomain;
    kernel::uint16_t reserved1;
    kernel::uint32_t baseAddressLow;
    kernel::uint32_t baseAddressHigh;
    kernel::uint32_t lengthLow;
    kernel::uint32_t lengthHigh;
    kernel::uint32_t reserved2;
    kernel::uint32_t flags;
    kernel::uint64_t reserved3;
} __attribute__((packed));

constexpr kernel::uint8_t kSratTypeProcessorApicAffinity = 0;
constexpr kernel::uint8_t kSratTypeMemoryAffinity = 1;
constexpr kernel::uint32_t kSratAffinityEnabledFlag = 1U << 0;
constexpr kernel::uint32_t kSratHeaderPad = 12;  // tableRevision(4) + reserved(8)

// ACPI HPET 테이블(IA-PC HPET 규격 3.2.4) - SdtHeader(36바이트) 바로
// 뒤에 이어진다. Generic Address Structure(주소공간ID+너비+비트오프셋
// +예약+실제주소, 12바이트)에서 실제 주소만 쓴다(항상 시스템 메모리
// 공간이라 addressSpaceId 검사는 생략 - 다른 값이 실무에서 쓰이는
// 사례가 없음).
struct HpetTable {
    SdtHeader header;
    kernel::uint32_t eventTimerBlockId;
    kernel::uint8_t addressSpaceId;
    kernel::uint8_t registerBitWidth;
    kernel::uint8_t registerBitOffset;
    kernel::uint8_t reserved0;
    kernel::uint64_t address;
    kernel::uint8_t hpetNumber;
    kernel::uint16_t minimumTick;
    kernel::uint8_t pageProtection;
} __attribute__((packed));

// ACPI MCFG(PCI Firmware Spec 3.0 §4.1.2) - SdtHeader(36바이트) +
// 예약 8바이트 뒤에 가변 개수의 엔트리가 이어진다. 세그먼트 그룹 0
// (첫 엔트리)만 다룬다.
struct McfgEntry {
    kernel::uint64_t baseAddress;
    kernel::uint16_t pciSegmentGroup;
    kernel::uint8_t startBusNumber;
    kernel::uint8_t endBusNumber;
    kernel::uint32_t reserved;
} __attribute__((packed));
constexpr kernel::uint32_t kMcfgHeaderPad = 8;  // 예약 필드

// ACPI FADT("FACP" 시그니처, ACPI 스펙 §5.2.9) - SdtHeader(36바이트)
// 바로 뒤. ACPI 1.0(리비전0/1)부터 있던 32비트 필드(오프셋 0~108)와
// ACPI 2.0+가 추가한 64비트 확장(X_*, 오프셋 96~) 둘 다 정의해 두되,
// 확장 필드는 `header.length`가 실제로 그만큼 있을 때만 읽는다(RSDP
// revision 분기와 동일한 방어 - 짧은 FADT를 읽으면 그 뒤는 다른
// 테이블의 메모리를 침범해 읽는 것과 같다). GAS(Generic Address
// Structure, HpetTable의 것과 동일한 12바이트 레이아웃)를 재사용하지
// 않고 그대로 인라인한다 - HpetTable의 것과 필드 이름이 겹쳐
// 헷갈리지 않도록.
struct GenericAddress {
    kernel::uint8_t addressSpaceId;
    kernel::uint8_t registerBitWidth;
    kernel::uint8_t registerBitOffset;
    kernel::uint8_t accessSize;
    kernel::uint64_t address;
} __attribute__((packed));
static_assert(sizeof(GenericAddress) == 12, "GenericAddress는 ACPI GAS와 같은 12바이트여야 함");

struct Fadt {
    SdtHeader header;              // 0
    kernel::uint32_t firmwareCtrl; // 36
    kernel::uint32_t dsdt;         // 40
    kernel::uint8_t reserved0;     // 44
    kernel::uint8_t preferredPmProfile;  // 45
    kernel::uint16_t sciInt;       // 46
    kernel::uint32_t smiCmd;       // 48
    kernel::uint8_t acpiEnable;    // 52
    kernel::uint8_t acpiDisable;   // 53
    kernel::uint8_t s4BiosReq;     // 54
    kernel::uint8_t pstateCnt;     // 55
    kernel::uint32_t pm1aEvtBlk;   // 56
    kernel::uint32_t pm1bEvtBlk;   // 60
    kernel::uint32_t pm1aCntBlk;   // 64
    kernel::uint32_t pm1bCntBlk;   // 68
    kernel::uint32_t pm2CntBlk;    // 72
    kernel::uint32_t pmTmrBlk;     // 76
    kernel::uint32_t gpe0Blk;      // 80
    kernel::uint32_t gpe1Blk;      // 84
    kernel::uint8_t pm1EvtLen;     // 88
    kernel::uint8_t pm1CntLen;     // 89
    kernel::uint8_t pm2CntLen;     // 90
    kernel::uint8_t pmTmrLen;      // 91
    kernel::uint8_t gpe0BlkLen;    // 92
    kernel::uint8_t gpe1BlkLen;    // 93
    kernel::uint8_t gpe1Base;      // 94
    kernel::uint8_t cstCnt;        // 95
    kernel::uint16_t pLvl2Lat;     // 96
    kernel::uint16_t pLvl3Lat;     // 98
    kernel::uint16_t flushSize;    // 100
    kernel::uint16_t flushStride;  // 102
    kernel::uint8_t dutyOffset;    // 104
    kernel::uint8_t dutyWidth;     // 105
    kernel::uint8_t dayAlrm;       // 106
    kernel::uint8_t monAlrm;       // 107
    kernel::uint8_t century;       // 108
    kernel::uint16_t iapcBootArch; // 109
    kernel::uint8_t reserved1;     // 111
    kernel::uint32_t flags;        // 112
    GenericAddress resetReg;       // 116
    kernel::uint8_t resetValue;    // 128
    kernel::uint16_t archBootFlags2;  // 129 (ARM_BOOT_ARCH, x86은 안 씀)
    kernel::uint8_t reserved2;     // 131
    kernel::uint64_t xFirmwareCtrl;  // 132
    kernel::uint64_t xDsdt;        // 140
    GenericAddress xPm1aEvtBlk;   // 148
    GenericAddress xPm1bEvtBlk;   // 160
    GenericAddress xPm1aCntBlk;   // 172
    GenericAddress xPm1bCntBlk;   // 184
    // 이후 X_PM2_CNT_BLK 등은 이번 범위(shutdown/reboot에 필요한
    // PM1/Reset만) 밖이라 옮기지 않는다 - 위 SuperblockCore/InodeCore와
    // 같은 절단 관례.
} __attribute__((packed));
static_assert(sizeof(SdtHeader) == 36, "SdtHeader는 36바이트여야 함(FADT 오프셋 계산의 기준)");
static_assert(__builtin_offsetof(Fadt, sciInt) == 46, "Fadt::sciInt 오프셋이 ACPI 스펙과 어긋남");
static_assert(__builtin_offsetof(Fadt, pm1aEvtBlk) == 56, "Fadt::pm1aEvtBlk 오프셋이 ACPI 스펙과 어긋남");
static_assert(__builtin_offsetof(Fadt, pm1CntLen) == 89, "Fadt::pm1CntLen 오프셋이 ACPI 스펙과 어긋남");
static_assert(__builtin_offsetof(Fadt, flags) == 112, "Fadt::flags 오프셋이 ACPI 스펙과 어긋남");
static_assert(__builtin_offsetof(Fadt, resetReg) == 116, "Fadt::resetReg 오프셋이 ACPI 스펙과 어긋남");
static_assert(__builtin_offsetof(Fadt, resetValue) == 128, "Fadt::resetValue 오프셋이 ACPI 스펙과 어긋남");
static_assert(__builtin_offsetof(Fadt, xDsdt) == 140, "Fadt::xDsdt 오프셋이 ACPI 스펙과 어긋남");
static_assert(__builtin_offsetof(Fadt, xPm1aCntBlk) == 172, "Fadt::xPm1aCntBlk 오프셋이 ACPI 스펙과 어긋남");

constexpr kernel::uint32_t kFadtFlagResetRegSup = 1U << 10;
// FADT가 이 길이(RESET_REG/RESET_VALUE까지, ACPI 2.0 최소 크기)보다
// 짧으면 리비전이 그 확장을 아예 정의하지 않는다는 뜻 - 읽으면 안 됨.
constexpr kernel::uint32_t kFadtLengthWithResetReg = 129;
// X_DSDT/X_PM1a_CNT_BLK 등 64비트 확장 전체(이 struct가 옮긴 필드
// 기준 - X_PM1b_CNT_BLK 끝, 오프셋 196)를 담을 최소 길이. 실제
// ACPI 2.0+ 펌웨어는 보통 FADT 전체 길이를 244(ACPI 5.0 정의 전체
// 크기)로 보고하지만, 이 struct는 그 뒤(HYPERVISOR_VENDOR_IDENTITY
// 등)를 옮기지 않았으므로 실제로 필요한 최소값만 기준으로 삼는다
// (SuperblockCore/InodeCore와 같은 절단 관례 - RM-23F4B687 §4).
constexpr kernel::uint32_t kFadtLengthWithExtended = 196;
// SCI_INT~PM1_CNT_LEN(오프셋 46~89)까지 - PM1/SCI/DSDT(32비트) 등
// 이 코드가 실제로 읽는 ACPI 1.0 시절부터 있던 최소 필드 범위.
constexpr kernel::uint32_t kFadtLengthWithPm1Core = 90;

kernel::uint64_t gLocalApicAddress = 0;
kernel::uint32_t gIoApicIds[kernel::kAcpiMaxIoApics];
kernel::uint32_t gIoApicAddresses[kernel::kAcpiMaxIoApics];
kernel::uint32_t gIoApicGsiBases[kernel::kAcpiMaxIoApics];
kernel::uint32_t gIoApicCount = 0;
bool gHasHpet = false;
kernel::uint64_t gHpetAddress = 0;
bool gHasMcfg = false;
kernel::uint64_t gMcfgBaseAddress = 0;
kernel::uint8_t gMcfgStartBus = 0;
kernel::uint8_t gMcfgEndBus = 0;
kernel::uint32_t gCpuApicIds[kernel::kAcpiMaxCpus];
kernel::uint32_t gCpuNumaNode[kernel::kAcpiMaxCpus];
kernel::uint32_t gCpuCount = 0;

// [신규, 2026-09-23, DC-F367AD5D/QU-7C3AB7A2 답변] FADT에서 옮긴
// PM1/Reset/DSDT 위치 - 전부 `Power`(power.h/.cpp)가 실제 하드웨어
// 조작에 쓴다. hasFadt()==false면 나머지는 전부 의미 없음(0으로
// 초기화된 채로 남는다 - 실제 물리 하드웨어에서 FADT가 없는 경우는
// 사실상 없지만, 방어적으로 항상 먼저 확인).
bool gHasFadt = false;
kernel::uint32_t gSciInt = 0;
kernel::uint32_t gSmiCmd = 0;
kernel::uint8_t gAcpiEnable = 0;
kernel::uint8_t gAcpiDisable = 0;
kernel::uint32_t gPm1aEvtBlk = 0;
kernel::uint32_t gPm1bEvtBlk = 0;
kernel::uint32_t gPm1EvtLen = 0;
kernel::uint32_t gPm1aCntBlk = 0;
kernel::uint32_t gPm1bCntBlk = 0;
kernel::uint32_t gPm1CntLen = 0;
bool gHasResetReg = false;
kernel::uint8_t gResetRegAddressSpaceId = 0;
kernel::uint64_t gResetRegAddress = 0;
kernel::uint8_t gResetValue = 0;
kernel::uint64_t gDsdtPhysAddress = 0;
kernel::uint32_t gDsdtLength = 0;

// ISA IRQ(인덱스) -> override 존재 여부/GSI/극성/트리거. override가
// 없는 IRQ는 gIsoPresent[irq]==false로 남고, Acpi::resolveIsaIrq가
// ISA 기본값(GSI=IRQ, active-high, edge)으로 채워 돌려준다.
bool gIsoPresent[kernel::kAcpiMaxIsoEntries];
kernel::uint32_t gIsoGsi[kernel::kAcpiMaxIsoEntries];
kernel::uint32_t gIsoPolarity[kernel::kAcpiMaxIsoEntries];
kernel::uint32_t gIsoTriggerMode[kernel::kAcpiMaxIsoEntries];

kernel::uint32_t gDomainValues[kernel::kAcpiMaxNumaNodes];
kernel::uint32_t gDomainCount = 0;

struct MemAffinity {
    kernel::uint64_t base;
    kernel::uint64_t length;
    kernel::uint32_t node;
};
MemAffinity gMemAffinities[kernel::kAcpiMaxMemoryAffinityEntries];
kernel::uint32_t gMemAffinityCount = 0;

template <typename T>
const T* kAsTable(kernel::uint64_t physAddr) {
    return reinterpret_cast<const T*>(kernel::kPhysToVirt(physAddr));
}

bool kChecksumOk(const void* data, kernel::uint32_t length) {
    kernel::uint8_t sum = 0;
    const auto* bytes = reinterpret_cast<const kernel::uint8_t*>(data);
    for (kernel::uint32_t i = 0; i < length; ++i) {
        sum = static_cast<kernel::uint8_t>(sum + bytes[i]);
    }
    return sum == 0;
}

bool kSignatureIs(const char* sig, const char* expected, kernel::int32_t len) {
    for (kernel::int32_t i = 0; i < len; ++i) {
        if (sig[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

// proximity domain 값(펌웨어가 매긴 임의의 ID)을 0부터 시작하는
// 내부 노드 인덱스로 바꾼다 - 처음 보는 값이면 새 노드를 만든다.
kernel::uint32_t kNodeIndexForDomain(kernel::uint32_t domain) {
    for (kernel::uint32_t i = 0; i < gDomainCount; ++i) {
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
    const auto* base = reinterpret_cast<const kernel::uint8_t*>(madtHeader);
    const auto* localApicAddrField = reinterpret_cast<const kernel::uint32_t*>(base + sizeof(SdtHeader));
    gLocalApicAddress = *localApicAddrField;

    const kernel::uint8_t* entry = base + sizeof(SdtHeader) + 8;  // localApicAddress(4) + flags(4)
    const kernel::uint8_t* end = base + madtHeader->length;

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
            if (gIoApicCount < kernel::kAcpiMaxIoApics) {
                gIoApicIds[gIoApicCount] = ioapic->ioApicId;
                gIoApicAddresses[gIoApicCount] = ioapic->ioApicAddress;
                gIoApicGsiBases[gIoApicCount] = ioapic->globalSystemInterruptBase;
                ++gIoApicCount;
            }
        } else if (entryHeader->type == kMadtTypeInterruptSourceOverride) {
            const auto* iso = reinterpret_cast<const MadtInterruptSourceOverrideEntry*>(entry);
            if (iso->source < kernel::kAcpiMaxIsoEntries) {
                gIsoPresent[iso->source] = true;
                gIsoGsi[iso->source] = iso->globalSystemInterrupt;

                const kernel::uint32_t rawPolarity = iso->flags & kMpsFlagsPolarityMask;
                gIsoPolarity[iso->source] =
                    (rawPolarity == 0 || rawPolarity == kernel::kAcpiPolarityActiveHigh)
                        ? kernel::kAcpiPolarityActiveHigh
                        : kernel::kAcpiPolarityActiveLow;

                const kernel::uint32_t rawTrigger = (iso->flags & kMpsFlagsTriggerMask) >> kMpsFlagsTriggerShift;
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
    const auto* base = reinterpret_cast<const kernel::uint8_t*>(sratHeader);
    const kernel::uint8_t* entry = base + sizeof(SdtHeader) + kSratHeaderPad;
    const kernel::uint8_t* end = base + sratHeader->length;

    while (entry < end) {
        const auto* entryHeader = reinterpret_cast<const SratEntryHeader*>(entry);
        if (entryHeader->length == 0) {
            break;
        }
        if (entryHeader->type == kSratTypeProcessorApicAffinity) {
            const auto* p = reinterpret_cast<const SratProcessorApicAffinity*>(entry);
            if (p->flags & kSratAffinityEnabledFlag) {
                const kernel::uint32_t domain = p->proximityDomainLow |
                                             (static_cast<kernel::uint32_t>(p->proximityDomainHigh[0]) << 8) |
                                             (static_cast<kernel::uint32_t>(p->proximityDomainHigh[1]) << 16) |
                                             (static_cast<kernel::uint32_t>(p->proximityDomainHigh[2]) << 24);
                const kernel::uint32_t node = kNodeIndexForDomain(domain);
                for (kernel::uint32_t i = 0; i < gCpuCount; ++i) {
                    if (gCpuApicIds[i] == p->apicId) {
                        gCpuNumaNode[i] = node;
                        break;
                    }
                }
            }
        } else if (entryHeader->type == kSratTypeMemoryAffinity) {
            const auto* m = reinterpret_cast<const SratMemoryAffinity*>(entry);
            if ((m->flags & kSratAffinityEnabledFlag) && gMemAffinityCount < kernel::kAcpiMaxMemoryAffinityEntries) {
                const kernel::uint64_t memBase = (static_cast<kernel::uint64_t>(m->baseAddressHigh) << 32) | m->baseAddressLow;
                const kernel::uint64_t memLength = (static_cast<kernel::uint64_t>(m->lengthHigh) << 32) | m->lengthLow;
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
const SdtHeader* kFindTable(kernel::uint64_t sdtPhys, const char* signature) {
    const auto* header = kAsTable<SdtHeader>(sdtPhys);
    if (!kChecksumOk(header, header->length)) {
        return nullptr;
    }
    const auto* base = reinterpret_cast<const kernel::uint8_t*>(header);
    const auto* pointers = reinterpret_cast<const PointerType*>(base + sizeof(SdtHeader));
    const kernel::uint32_t count = (header->length - sizeof(SdtHeader)) / sizeof(PointerType);

    for (kernel::uint32_t i = 0; i < count; ++i) {
        const auto* candidate = kAsTable<SdtHeader>(static_cast<kernel::uint64_t>(pointers[i]));
        if (kSignatureIs(candidate->signature, signature, 4) && kChecksumOk(candidate, candidate->length)) {
            return candidate;
        }
    }
    return nullptr;
}

}  // namespace

namespace kernel {

bool Acpi::init(kernel::uint64_t rsdpPhys) {
    const auto* rsdp = kAsTable<Rsdp>(rsdpPhys);
    if (!kSignatureIs(rsdp->signature, "RSD PTR ", 8)) {
        return false;
    }

    const SdtHeader* madt = nullptr;
    const SdtHeader* srat = nullptr;
    const SdtHeader* hpet = nullptr;
    const SdtHeader* mcfg = nullptr;
    const SdtHeader* fadt = nullptr;

    if (rsdp->revision >= 2 && kChecksumOk(rsdp, sizeof(Rsdp)) && rsdp->xsdtAddress) {
        madt = kFindTable<kernel::uint64_t>(rsdp->xsdtAddress, "APIC");
        srat = kFindTable<kernel::uint64_t>(rsdp->xsdtAddress, "SRAT");
        hpet = kFindTable<kernel::uint64_t>(rsdp->xsdtAddress, "HPET");
        mcfg = kFindTable<kernel::uint64_t>(rsdp->xsdtAddress, "MCFG");
        fadt = kFindTable<kernel::uint64_t>(rsdp->xsdtAddress, "FACP");
    } else if (kChecksumOk(rsdp, 20)) {  // ACPI 1.0 RSDP는 처음 20바이트만 체크섬 대상
        madt = kFindTable<kernel::uint32_t>(rsdp->rsdtAddress, "APIC");
        srat = kFindTable<kernel::uint32_t>(rsdp->rsdtAddress, "SRAT");
        hpet = kFindTable<kernel::uint32_t>(rsdp->rsdtAddress, "HPET");
        mcfg = kFindTable<kernel::uint32_t>(rsdp->rsdtAddress, "MCFG");
        fadt = kFindTable<kernel::uint32_t>(rsdp->rsdtAddress, "FACP");
    } else {
        return false;
    }

    if (fadt && fadt->length >= kFadtLengthWithPm1Core) {
        const auto* f = reinterpret_cast<const Fadt*>(fadt);
        gHasFadt = true;
        gSciInt = f->sciInt;
        gSmiCmd = f->smiCmd;
        gAcpiEnable = f->acpiEnable;
        gAcpiDisable = f->acpiDisable;
        gPm1aEvtBlk = f->pm1aEvtBlk;
        gPm1bEvtBlk = f->pm1bEvtBlk;
        gPm1EvtLen = f->pm1EvtLen;
        gPm1aCntBlk = f->pm1aCntBlk;
        gPm1bCntBlk = f->pm1bCntBlk;
        gPm1CntLen = f->pm1CntLen;
        gDsdtPhysAddress = f->dsdt;

        if (fadt->length >= kFadtLengthWithResetReg && (f->flags & kFadtFlagResetRegSup)) {
            gHasResetReg = true;
            gResetRegAddressSpaceId = f->resetReg.addressSpaceId;
            gResetRegAddress = f->resetReg.address;
            gResetValue = f->resetValue;
        }

        // ACPI 2.0+는 64비트 X_DSDT를 우선 사용(스펙 권고 - 32비트
        // DSDT 필드는 하위호환용). 0이면 확장 필드 자체가 안 채워진
        // 것으로 보고 32비트 값을 그대로 유지한다.
        if (fadt->length >= kFadtLengthWithExtended && f->xDsdt != 0) {
            gDsdtPhysAddress = f->xDsdt;
        }
        if (gDsdtPhysAddress != 0) {
            const auto* dsdtHeader = kAsTable<SdtHeader>(gDsdtPhysAddress);
            if (kSignatureIs(dsdtHeader->signature, "DSDT", 4) && kChecksumOk(dsdtHeader, dsdtHeader->length)) {
                gDsdtLength = dsdtHeader->length;
            } else {
                gDsdtPhysAddress = 0;  // 손상/불일치 - Power가 "S5 정보 없음"으로 취급하도록
            }
        }
    }

    if (hpet) {
        gHasHpet = true;
        gHpetAddress = reinterpret_cast<const HpetTable*>(hpet)->address;
    }

    if (mcfg && mcfg->length >= sizeof(SdtHeader) + kMcfgHeaderPad + sizeof(McfgEntry)) {
        const auto* mcfgBase = reinterpret_cast<const kernel::uint8_t*>(mcfg);
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
        for (kernel::uint32_t i = 0; i < gCpuCount; ++i) {
            gCpuNumaNode[i] = 0;
        }
    }
    return true;
}

kernel::uint64_t Acpi::localApicAddress() { return gLocalApicAddress; }
kernel::uint32_t Acpi::cpuCount() { return gCpuCount; }
kernel::uint32_t Acpi::cpuApicId(kernel::uint32_t index) { return gCpuApicIds[index]; }
kernel::uint32_t Acpi::ioApicCount() { return gIoApicCount; }
kernel::uint32_t Acpi::ioApicId(kernel::uint32_t index) { return gIoApicIds[index]; }
kernel::uint32_t Acpi::ioApicAddress(kernel::uint32_t index) { return gIoApicAddresses[index]; }
kernel::uint32_t Acpi::ioApicGsiBase(kernel::uint32_t index) { return gIoApicGsiBases[index]; }

Acpi::IsaIrqRouting Acpi::resolveIsaIrq(kernel::uint32_t isaIrq) {
    if (isaIrq < kAcpiMaxIsoEntries && gIsoPresent[isaIrq]) {
        return {gIsoGsi[isaIrq], gIsoPolarity[isaIrq], gIsoTriggerMode[isaIrq]};
    }
    // override 없음 - ACPI 스펙의 ISA 기본값(GSI=IRQ, active-high, edge).
    return {isaIrq, kAcpiPolarityActiveHigh, kAcpiTriggerEdge};
}

bool Acpi::hasHpet() { return gHasHpet; }
kernel::uint64_t Acpi::hpetAddress() { return gHpetAddress; }

bool Acpi::hasMcfg() { return gHasMcfg; }
kernel::uint64_t Acpi::mcfgBaseAddress() { return gMcfgBaseAddress; }
kernel::uint8_t Acpi::mcfgStartBus() { return gMcfgStartBus; }
kernel::uint8_t Acpi::mcfgEndBus() { return gMcfgEndBus; }

kernel::uint32_t Acpi::numaNodeCount() { return gDomainCount; }
kernel::uint32_t Acpi::cpuNumaNode(kernel::uint32_t index) { return gCpuNumaNode[index]; }

kernel::uint32_t Acpi::memoryAffinityCount() { return gMemAffinityCount; }
kernel::uint64_t Acpi::memoryAffinityBase(kernel::uint32_t index) { return gMemAffinities[index].base; }
kernel::uint64_t Acpi::memoryAffinityLength(kernel::uint32_t index) { return gMemAffinities[index].length; }
kernel::uint32_t Acpi::memoryAffinityNode(kernel::uint32_t index) { return gMemAffinities[index].node; }

bool Acpi::hasFadt() { return gHasFadt; }
kernel::uint32_t Acpi::sciInterruptGsi() { return gSciInt; }
kernel::uint32_t Acpi::smiCommandPort() { return gSmiCmd; }
kernel::uint8_t Acpi::acpiEnableValue() { return gAcpiEnable; }
kernel::uint8_t Acpi::acpiDisableValue() { return gAcpiDisable; }
kernel::uint32_t Acpi::pm1aEventBlock() { return gPm1aEvtBlk; }
kernel::uint32_t Acpi::pm1bEventBlock() { return gPm1bEvtBlk; }
kernel::uint32_t Acpi::pm1EventBlockLength() { return gPm1EvtLen; }
kernel::uint32_t Acpi::pm1aControlBlock() { return gPm1aCntBlk; }
kernel::uint32_t Acpi::pm1bControlBlock() { return gPm1bCntBlk; }
kernel::uint32_t Acpi::pm1ControlBlockLength() { return gPm1CntLen; }

bool Acpi::hasResetRegister() { return gHasResetReg; }
kernel::uint8_t Acpi::resetRegisterAddressSpaceId() { return gResetRegAddressSpaceId; }
kernel::uint64_t Acpi::resetRegisterAddress() { return gResetRegAddress; }
kernel::uint8_t Acpi::resetRegisterValue() { return gResetValue; }

kernel::uint64_t Acpi::dsdtPhysAddress() { return gDsdtPhysAddress; }
kernel::uint32_t Acpi::dsdtLength() { return gDsdtLength; }

}  // namespace kernel
