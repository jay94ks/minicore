#include "gdt.h"

#include "acpi.h"
#include "lapic.h"
#include "libkenv/types.h"

namespace {

// boot.S의 gdt64(code64/data) 디스크립터 값과 완전히 동일하게 맞춘다 -
// GDTR을 바꿔도 CPU가 이미 로드된 CS의 히든 캐시를 스스로 재적재하진
// 않으므로, 같은 셀렉터 위치에 같은 내용이 있어야 far jump 없이도
// 안전하게 lgdt만으로 갈아탈 수 있다(boot.S 참고).
constexpr kernel::uint64_t kNullDescriptor = 0;
constexpr kernel::uint64_t kCode64Descriptor = 0x00AF9A000000FFFFULL;
constexpr kernel::uint64_t kData64Descriptor = 0x00CF92000000FFFFULL;

constexpr kernel::uint32_t kMaxCores = kernel::kAcpiMaxCpus;
constexpr kernel::uint64_t kIst1StackSize = 8192;  // #DF 전용 - Task 기본 커널 스택(8KiB)과 동일 크기

// GDT 배치: [0]=null, [1]=code64, [2]=data64(각 8바이트), 그 뒤로
// 코어마다 16바이트짜리 TSS 디스크립터가 하나씩 이어진다 - 셀렉터
// 계산이 쉽도록 고정 오프셋(3*8=0x18)부터 시작한다.
constexpr kernel::uint32_t kFixedEntryCount = 3;
constexpr kernel::uint32_t kTssDescriptorBaseOffset = kFixedEntryCount * 8;  // 0x18

alignas(16) kernel::uint64_t gGdt[kFixedEntryCount + 2 * kMaxCores];

struct GdtPointer {
    kernel::uint16_t limit;
    kernel::uint64_t base;
} __attribute__((packed));

GdtPointer gGdtPointer;

// x86_64 TSS(Intel SDM Vol.3 8.7, Figure 8-11) - packed라야 스펙과
// 정확히 같은 오프셋/104바이트 크기가 나온다.
struct Tss {
    kernel::uint32_t reserved0;
    kernel::uint64_t rsp0;
    kernel::uint64_t rsp1;
    kernel::uint64_t rsp2;
    kernel::uint64_t reserved1;
    kernel::uint64_t ist1;
    kernel::uint64_t ist2;
    kernel::uint64_t ist3;
    kernel::uint64_t ist4;
    kernel::uint64_t ist5;
    kernel::uint64_t ist6;
    kernel::uint64_t ist7;
    kernel::uint64_t reserved2;
    kernel::uint16_t reserved3;
    kernel::uint16_t ioMapBase;
} __attribute__((packed));

static_assert(sizeof(Tss) == 104, "x86_64 TSS 레이아웃이 SDM Vol.3 Figure 8-11과 정확히 일치해야 한다");

Tss gTssPerCore[kMaxCores];
alignas(16) kernel::uint8_t gIst1Stacks[kMaxCores][kIst1StackSize];

struct TssDescriptorPair {
    kernel::uint64_t low;
    kernel::uint64_t high;
};

// 롱모드 TSS 디스크립터(16바이트, 시스템 세그먼트) - 하위 8바이트는
// 표준 세그먼트 디스크립터 형식(type=0x9: 사용가능한 64비트 TSS),
// 상위 8바이트는 base[63:32](나머지는 예약, 0).
TssDescriptorPair kMakeTssDescriptor(kernel::uint64_t base, kernel::uint32_t limit) {
    kernel::uint64_t low = 0;
    low |= static_cast<kernel::uint64_t>(limit & 0xFFFF);
    low |= (base & 0xFFFFFFUL) << 16;
    low |= 0x9ULL << 40;  // type = 1001(64비트 TSS, available)
    low |= 1ULL << 47;    // P = 1
    low |= (static_cast<kernel::uint64_t>(limit >> 16) & 0xF) << 48;
    low |= ((base >> 24) & 0xFFULL) << 56;

    const kernel::uint64_t high = (base >> 32) & 0xFFFFFFFFULL;
    return {low, high};
}

void kSetTssDescriptor(kernel::uint32_t coreIndex, kernel::uint64_t base, kernel::uint32_t limit) {
    const TssDescriptorPair pair = kMakeTssDescriptor(base, limit);
    const kernel::uint32_t qwordIndex = kFixedEntryCount + coreIndex * 2;
    gGdt[qwordIndex] = pair.low;
    gGdt[qwordIndex + 1] = pair.high;
}

// GDTR을 gGdt로 (다시) 적재하고 데이터 세그먼트 레지스터를 방어적으로
// 다시 로드한다 - 코드 셀렉터(0x08)는 boot.S의 gdt64와 내용이 완전히
// 같아 재적재가 필요 없다(Gdt::init 위 주석 참고).
void kLoadGdtOnThisCore() {
    asm volatile("lgdt %0" : : "m"(gGdtPointer));
    asm volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        :
        :
        : "ax");
}

}  // namespace

namespace kernel {

uint16_t Gdt::tssSelectorForCore(uint32_t coreIndex) {
    return static_cast<uint16_t>(kTssDescriptorBaseOffset + coreIndex * 16);
}

void Gdt::init() {
    gGdt[0] = kNullDescriptor;
    gGdt[1] = kCode64Descriptor;
    gGdt[2] = kData64Descriptor;

    for (uint32_t i = 0; i < kMaxCores; ++i) {
        kSetTssDescriptor(i, reinterpret_cast<uint64_t>(&gTssPerCore[i]), sizeof(Tss) - 1);
    }

    gGdtPointer.limit = static_cast<uint16_t>(sizeof(gGdt) - 1);
    gGdtPointer.base = reinterpret_cast<uint64_t>(&gGdt[0]);
    kLoadGdtOnThisCore();
}

void Gdt::reloadOnThisCore() {
    kLoadGdtOnThisCore();
}

void Gdt::loadTssForThisCore() {
    const uint32_t apicId = Lapic::id();
    uint32_t coreIndex = 0;
    const uint32_t cpuCount = Acpi::cpuCount();
    for (uint32_t i = 0; i < cpuCount; ++i) {
        if (Acpi::cpuApicId(i) == apicId) {
            coreIndex = i;
            break;
        }
    }
    if (coreIndex >= kMaxCores) {
        coreIndex = 0;  // 방어적 fallback(kAcpiMaxCpus 상한 초과 - 있을 수 없는 경우)
    }

    Tss& tss = gTssPerCore[coreIndex];
    tss.ist1 = reinterpret_cast<uint64_t>(&gIst1Stacks[coreIndex][kIst1StackSize]);
    tss.ioMapBase = sizeof(Tss);  // IOPB 없음 - 세그먼트 한계를 벗어나게 해 비활성화

    const uint16_t selector = Gdt::tssSelectorForCore(coreIndex);
    asm volatile("ltr %0" : : "r"(selector));
}

}  // namespace kernel
