#include "multiboot2.h"

namespace {

struct TagHeader {
    unsigned int type;
    unsigned int size;
} __attribute__((packed));

constexpr unsigned int kTagTypeEnd = 0;
constexpr unsigned int kTagTypeMemoryMap = 6;
constexpr unsigned int kTagTypeAcpiOldRsdp = 14;
constexpr unsigned int kTagTypeAcpiNewRsdp = 15;

struct MemoryMapTag {
    TagHeader header;
    unsigned int entrySize;
    unsigned int entryVersion;
} __attribute__((packed));

struct MemoryMapEntryRaw {
    unsigned long baseAddr;
    unsigned long length;
    unsigned int type;
    unsigned int reserved;
} __attribute__((packed));

// 멀티부트2 메모리맵 태그의 타입 값은 E820과 거의 같다(1=usable,
// 3=ACPI reclaimable, 4=ACPI NVS, 5=defective, 그 외 전부 예약) -
// kernel::HvmMemmapType과 그대로 맞춰 변환한다.
unsigned int kMapMemType(unsigned int mb2Type) {
    switch (mb2Type) {
        case 1:
            return static_cast<unsigned int>(kernel::HvmMemmapType::kUsable);
        case 3:
            return static_cast<unsigned int>(kernel::HvmMemmapType::kAcpiReclaimable);
        case 4:
            return static_cast<unsigned int>(kernel::HvmMemmapType::kAcpiNvs);
        case 5:
            return static_cast<unsigned int>(kernel::HvmMemmapType::kUnusable);
        default:
            return static_cast<unsigned int>(kernel::HvmMemmapType::kReserved);
    }
}

}  // namespace

namespace kernel {

void Multiboot2Info::parse(unsigned long infoPhysAddr, HvmMemmapEntry* outMemmap, unsigned int maxEntries,
                            unsigned int* outMemmapCount, unsigned long* outRsdpPaddr, unsigned int* outTotalSize) {
    const auto* base = reinterpret_cast<const unsigned char*>(infoPhysAddr);
    const unsigned int totalSize = *reinterpret_cast<const unsigned int*>(base);
    *outTotalSize = totalSize;
    *outMemmapCount = 0;
    *outRsdpPaddr = 0;

    unsigned long oldRsdpPhys = 0;
    const unsigned char* tagPtr = base + 8;  // total_size(4)+reserved(4) 헤더 다음부터 태그 시작
    const unsigned char* end = base + totalSize;

    while (tagPtr + sizeof(TagHeader) <= end) {
        const auto* tag = reinterpret_cast<const TagHeader*>(tagPtr);
        if (tag->type == kTagTypeEnd) {
            break;
        }
        if (tag->type == kTagTypeMemoryMap) {
            const auto* mmTag = reinterpret_cast<const MemoryMapTag*>(tagPtr);
            const unsigned char* entryPtr = tagPtr + sizeof(MemoryMapTag);
            const unsigned char* entryEnd = tagPtr + mmTag->header.size;
            while (entryPtr + sizeof(MemoryMapEntryRaw) <= entryEnd && *outMemmapCount < maxEntries) {
                const auto* entry = reinterpret_cast<const MemoryMapEntryRaw*>(entryPtr);
                HvmMemmapEntry& out = outMemmap[*outMemmapCount];
                out.addr = entry->baseAddr;
                out.size = entry->length;
                out.type = kMapMemType(entry->type);
                out.reserved = 0;
                ++(*outMemmapCount);
                entryPtr += mmTag->entrySize;
            }
        } else if (tag->type == kTagTypeAcpiNewRsdp) {
            *outRsdpPaddr = reinterpret_cast<unsigned long>(tagPtr + sizeof(TagHeader));
        } else if (tag->type == kTagTypeAcpiOldRsdp && oldRsdpPhys == 0) {
            oldRsdpPhys = reinterpret_cast<unsigned long>(tagPtr + sizeof(TagHeader));
        }

        const unsigned int advance = (tag->size + 7) & ~7U;  // 태그는 8바이트 경계로 패딩된다(스펙)
        if (advance == 0) {
            break;  // 손상 방어 - 무한루프 방지
        }
        tagPtr += advance;
    }

    if (*outRsdpPaddr == 0) {
        *outRsdpPaddr = oldRsdpPhys;  // 신규(v2) RSDP 태그가 없으면 구형(v1)으로 폴백
    }
}

}  // namespace kernel
