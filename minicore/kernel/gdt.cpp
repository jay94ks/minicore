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

// ring3 유저 코드/데이터 디스크립터(PN-124C105B) - 커널 것과 완전히
// 같은 비트 패턴에서 access byte의 DPL 필드만 0(00)->3(11)으로 바꾼
// 값이다(access byte 0x9A/0x92 -> 0xFA/0xF2, 그 외 base/limit/flags는
// 전부 동일 - 롱모드에선 실질적으로 무시되는 필드들이다). 배치 순서/
// 자리(0x18=데이터, 0x20=코드)는 gdt.h의 kGdtUserDataSelector/
// kGdtUserCodeSelector 주석 참고(SYSRET 규약).
constexpr kernel::uint64_t kUserDataDescriptor = 0x00CFF2000000FFFFULL;
constexpr kernel::uint64_t kUserCodeDescriptor = 0x00AFFA000000FFFFULL;

// SYSRET의 STAR[63:48]+8/+16 규약이 정확히 이 GDT 오프셋을 가리키게
// 될 것이므로(향후 syscall MSR 설정 시점의 전제), 이 배치가 실수로
// 바뀌면 여기서 바로 걸리게 해 둔다.
static_assert((kernel::kGdtUserDataSelector & ~0x3) == 0x18,
              "kGdtUserDataSelector는 GDT 오프셋 0x18에 있어야 한다(SYSRET 규약)");
static_assert((kernel::kGdtUserCodeSelector & ~0x3) == 0x20,
              "kGdtUserCodeSelector는 GDT 오프셋 0x20에 있어야 한다(SYSRET 규약)");

constexpr kernel::uint32_t kMaxCores = kernel::kAcpiMaxCpus;
constexpr kernel::uint64_t kIstStackSize = 8192;  // Task 기본 커널 스택(8KiB)과 동일 크기

// IST1-4를 이 순서로 쓴다(idt.cpp의 kDoubleFaultIst/kNmiIst/
// kMachineCheckIst/kDebugIst와 정확히 대응) - #DF/NMI/#MC/#DB 넷 다
// "현재 RSP가 뭐든 무관하게 항상 유효한 스택이 필요한" 벡터라
// 코어마다 각자의 전용 스택을 하나씩 받는다(DC-3D3212A4/QU-4E00C118
// 후속 지시, 2026-09-14). IST5-7은 아직 안 쓴다 - 필요해지면 이
// 배열을 늘리고 idt.cpp에 같은 패턴으로 추가한다.
constexpr kernel::uint32_t kIstSlotCount = 4;

// GDT 배치: [0]=null, [1]=code64(ring0), [2]=data64(ring0),
// [3]=data(ring3, 0x18), [4]=code64(ring3, 0x20)(각 8바이트) - 유저
// 세그먼트 두 개를 이 순서/자리에 두는 이유는 gdt.h 상단 주석 참고
// (SYSRET 규약). 그 뒤로 코어마다 16바이트짜리 TSS 디스크립터가
// 하나씩 이어진다 - 셀렉터 계산이 쉽도록 고정 오프셋(5*8=0x28)부터
// 시작한다.
constexpr kernel::uint32_t kFixedEntryCount = 5;
constexpr kernel::uint32_t kTssDescriptorBaseOffset = kFixedEntryCount * 8;  // 0x28

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

// gIstStacks[slot][coreIndex]가 IST(slot+1)용 스택이다(슬롯 0=IST1
// #DF, 1=IST2 NMI, 2=IST3 #MC, 3=IST4 #DB - 위 주석 참고).
alignas(16) kernel::uint8_t gIstStacks[kIstSlotCount][kMaxCores][kIstStackSize];

kernel::uint64_t kIstStackTop(kernel::uint32_t slot, kernel::uint32_t coreIndex) {
    return reinterpret_cast<kernel::uint64_t>(&gIstStacks[slot][coreIndex][kIstStackSize]);
}

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

// loadTssForThisCore()/Gdt::setRsp0ForThisCore() 공용 - 이 코어의
// Lapic::id()를 Acpi::cpuApicId() 배열에서 역산해 코어 인덱스를 찾는다
// (page_frame_allocator.cpp의 kCurrentNumaNode()와 같은 패턴).
kernel::uint32_t kCoreIndexForTss() {
    const kernel::uint32_t apicId = kernel::Lapic::id();
    const kernel::uint32_t cpuCount = kernel::Acpi::cpuCount();
    for (kernel::uint32_t i = 0; i < cpuCount; ++i) {
        if (kernel::Acpi::cpuApicId(i) == apicId) {
            return (i < kMaxCores) ? i : 0;  // 방어적 fallback(kAcpiMaxCpus 상한 초과 - 있을 수 없는 경우)
        }
    }
    return 0;
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
    gGdt[3] = kUserDataDescriptor;
    gGdt[4] = kUserCodeDescriptor;

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
    const uint32_t coreIndex = kCoreIndexForTss();

    Tss& tss = gTssPerCore[coreIndex];
    tss.ist1 = kIstStackTop(0, coreIndex);  // #DF
    tss.ist2 = kIstStackTop(1, coreIndex);  // NMI
    tss.ist3 = kIstStackTop(2, coreIndex);  // #MC
    tss.ist4 = kIstStackTop(3, coreIndex);  // #DB
    tss.ioMapBase = sizeof(Tss);  // IOPB 없음 - 세그먼트 한계를 벗어나게 해 비활성화

    const uint16_t selector = Gdt::tssSelectorForCore(coreIndex);
    asm volatile("ltr %0" : : "r"(selector));
}

void Gdt::setRsp0ForThisCore(uint64_t rsp0) {
    gTssPerCore[kCoreIndexForTss()].rsp0 = rsp0;
}

}  // namespace kernel
