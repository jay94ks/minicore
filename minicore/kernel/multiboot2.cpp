#include "multiboot2.h"

#include "libkenv/types.h"

namespace {

struct TagHeader {
    kernel::uint32_t type;
    kernel::uint32_t size;
} __attribute__((packed));

constexpr kernel::uint32_t kTagTypeEnd = 0;
constexpr kernel::uint32_t kTagTypeCmdline = 1;
constexpr kernel::uint32_t kTagTypeBootloaderName = 2;
constexpr kernel::uint32_t kTagTypeModule = 3;
constexpr kernel::uint32_t kTagTypeMemoryMap = 6;
constexpr kernel::uint32_t kTagTypeAcpiOldRsdp = 14;
constexpr kernel::uint32_t kTagTypeAcpiNewRsdp = 15;

struct MemoryMapTag {
    TagHeader header;
    kernel::uint32_t entrySize;
    kernel::uint32_t entryVersion;
} __attribute__((packed));

struct MemoryMapEntryRaw {
    kernel::uint64_t baseAddr;
    kernel::uint64_t length;
    kernel::uint32_t type;
    kernel::uint32_t reserved;
} __attribute__((packed));

// 타입1(커맨드라인)/타입2(부트로더 이름)은 헤더 바로 뒤에 그냥
// null-terminated 문자열이 온다 - 별도 구조체 불필요.

struct ModuleTag {
    TagHeader header;
    kernel::uint32_t modStart;
    kernel::uint32_t modEnd;
    // 바로 뒤에 null-terminated 문자열(모듈 커맨드라인)이 이어진다.
} __attribute__((packed));

// 멀티부트2 메모리맵 태그의 타입 값은 E820과 거의 같다(1=usable,
// 3=ACPI reclaimable, 4=ACPI NVS, 5=defective, 그 외 전부 예약) -
// kernel::HvmMemmapType과 그대로 맞춰 변환한다.
kernel::uint32_t kMapMemType(kernel::uint32_t mb2Type) {
    switch (mb2Type) {
        case 1:
            return static_cast<kernel::uint32_t>(kernel::HvmMemmapType::kUsable);
        case 3:
            return static_cast<kernel::uint32_t>(kernel::HvmMemmapType::kAcpiReclaimable);
        case 4:
            return static_cast<kernel::uint32_t>(kernel::HvmMemmapType::kAcpiNvs);
        case 5:
            return static_cast<kernel::uint32_t>(kernel::HvmMemmapType::kUnusable);
        default:
            return static_cast<kernel::uint32_t>(kernel::HvmMemmapType::kReserved);
    }
}

}  // namespace

namespace kernel {

void Multiboot2Info::parse(uint64_t infoPhysAddr, HvmMemmapEntry* outMemmap, uint32_t maxEntries,
                            uint32_t* outMemmapCount, uint64_t* outRsdpPaddr, uint32_t* outTotalSize,
                            BootInfo* outBootInfo) {
    const auto* base = reinterpret_cast<const uint8_t*>(infoPhysAddr);
    const uint32_t totalSize = *reinterpret_cast<const uint32_t*>(base);
    *outTotalSize = totalSize;
    *outMemmapCount = 0;
    *outRsdpPaddr = 0;
    outBootInfo->cmdline = nullptr;
    outBootInfo->bootloaderName = nullptr;
    outBootInfo->moduleCount = 0;

    uint64_t oldRsdpPhys = 0;
    const uint8_t* tagPtr = base + 8;  // total_size(4)+reserved(4) 헤더 다음부터 태그 시작
    const uint8_t* end = base + totalSize;

    while (tagPtr + sizeof(TagHeader) <= end) {
        const auto* tag = reinterpret_cast<const TagHeader*>(tagPtr);
        if (tag->type == kTagTypeEnd) {
            break;
        }
        if (tag->type == kTagTypeMemoryMap) {
            const auto* mmTag = reinterpret_cast<const MemoryMapTag*>(tagPtr);
            const uint8_t* entryPtr = tagPtr + sizeof(MemoryMapTag);
            const uint8_t* entryEnd = tagPtr + mmTag->header.size;
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
            *outRsdpPaddr = reinterpret_cast<uint64_t>(tagPtr + sizeof(TagHeader));
        } else if (tag->type == kTagTypeAcpiOldRsdp && oldRsdpPhys == 0) {
            oldRsdpPhys = reinterpret_cast<uint64_t>(tagPtr + sizeof(TagHeader));
        } else if (tag->type == kTagTypeCmdline) {
            outBootInfo->cmdline = reinterpret_cast<const char*>(tagPtr + sizeof(TagHeader));
        } else if (tag->type == kTagTypeBootloaderName) {
            outBootInfo->bootloaderName = reinterpret_cast<const char*>(tagPtr + sizeof(TagHeader));
        } else if (tag->type == kTagTypeModule) {
            if (outBootInfo->moduleCount < kBootInfoMaxModules) {
                const auto* modTag = reinterpret_cast<const ModuleTag*>(tagPtr);
                BootModule& mod = outBootInfo->modules[outBootInfo->moduleCount];
                mod.physStart = modTag->modStart;
                mod.physEnd = modTag->modEnd;
                mod.cmdline = reinterpret_cast<const char*>(tagPtr + sizeof(ModuleTag));
                ++outBootInfo->moduleCount;
            }
        }

        const uint32_t advance = (tag->size + 7) & ~7U;  // 태그는 8바이트 경계로 패딩된다(스펙)
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
