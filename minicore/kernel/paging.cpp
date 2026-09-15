#include "paging.h"

#include "libkenv/types.h"
#include "page_frame_allocator.h"

namespace {

constexpr kernel::uint64_t kPageSize4K = 0x1000;
constexpr kernel::uint64_t kPageSize2M = 0x200000UL;
constexpr kernel::uint64_t kPageSize1G = 0x40000000UL;
constexpr kernel::uint64_t kAddrMask = 0x000FFFFFFFFFF000UL;  // 엔트리에서 플래그 비트 뻔 물리주소
constexpr kernel::uint64_t kPageSizeBit = 1UL << 7;           // PS(PDPT/PD 레벨 대형 페이지)
constexpr kernel::uint32_t kEntriesPerTable = 512;            // x86_64 4단계 페이징의 모든 테이블(PML4/PDPT/PD/PT)은 512엔트리 고정

// PL-57CF86EF 병합 불변 조건이 실제로 비교하는 leaf 엔트리 플래그
// 집합 - PRESENT를 포함해 두면 분할/병합 시 이 값을 그대로 leaf
// 엔트리에 다시 써넣는 것만으로 PRESENT 비트까지 함께 복원된다.
constexpr kernel::uint64_t kLeafFlagsMask =
    kernel::PAGE_PRESENT | kernel::PAGE_WRITABLE | kernel::PAGE_USER | kernel::PAGE_CACHE_DISABLE;

bool kIsAligned(kernel::uint64_t value, kernel::uint64_t align) {
    return (value & (align - 1)) == 0;
}

// 커널 higher-half의 시작 PML4 인덱스(Paging::createAddressSpace) -
// kDirectMapBase(0xFFFF800000000000)가 정확히 그 경계다(canonical
// 주소의 부호 확장 경계, bit 47). 인덱스 256~511(총 256개)을 통째로
// 복사하면 direct map/지연 매핑 구역/커널 이미지 자신(kKernelVma
// 근방)까지 전부 한 번에 커버된다 - 이 셋의 정확한 하위 배치를
// 개별적으로 알 필요가 없다.
constexpr kernel::uint32_t kHigherHalfPml4Start = 256;
constexpr kernel::uint32_t kPml4EntryCount = 512;

// direct map 1GiB 페이지 개수의 하한/상한(PN-4AA5425D) - 하한 4GiB는
// LAPIC(0xFEE00000)/IOAPIC/HPET 등 저지대 MMIO가 실제 설치 메모리
// 크기와 무관하게 항상 이 안에 있어야 하기 때문이고, 상한 512는 PDPT
// 하나(gDirectMapPdptStorage, 4096B = uint64_t 512개)가 가질 수 있는
// 엔트리 개수 자체의 물리적 한계다(더 늘리려면 PDPT를 여러 개 두는
// 구조 변경이 필요 - 후속 과제).
constexpr kernel::uint32_t kMinDirectMapGib = 4;
constexpr kernel::uint32_t kMaxDirectMapGib = 512;

// direct map용 PDPT 하나만 정적으로 예약한다(컴파일 타임 .bss, 커널
// 자신의 higher-half 이미지 안이라 이미 매핑돼 있다 - PageFrameAllocator
// 초기화 전에도 안전하게 쓸 수 있다). 첫 4GiB만 1GiB 페이지로 덮는다.
alignas(4096) kernel::uint8_t gDirectMapPdptStorage[4096];

// Paging::init()이 확정한 실제 direct map 범위(바이트) -
// PageFrameAllocator::init()이 Paging::directMapLimit()으로 읽어간다.
kernel::uint64_t gDirectMapLimit = 0;

kernel::uint64_t kCurrentPml4Phys() {
    kernel::uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & kAddrMask;
}

// Paging::init() 자신이 direct map을 만들기 전에 딜 한 번(자기 자신의
// PML4를 읽으려고) 쓴다 - 그 시점엔 kPhysToVirt를 아직 못 쓴다(direct
// map이 없으니까). boot.S가 PML4를 항상 저지대 identity map 구간에
// 두기 때문에 안전하다. **이 함수는 Paging::init() 밖에서 쓰면 안
// 된다** - PageFrameAllocator가 그 뒤로는 1GiB 밖 프레임도 내주므로
// (PL-99562483) identity 가정이 깨진다.
kernel::uint64_t* kLowIdentityTable(kernel::uint64_t physAddr) {
    return reinterpret_cast<kernel::uint64_t*>(physAddr);
}

// direct map(0~4GiB, Paging::init() 이후 항상 존재)을 거쳐 임의 물리
// 프레임을 가리키는 포인터를 얻는다 - PageFrameAllocator가 내주는
// 프레임이 이제 1GiB를 넘어설 수 있어(PL-99562483, 2026-09-14) 더는
// identity 캐스팅을 쓰면 안 된다(실측으로 페이지폴트 걸림 - -m 2048
// 환경에서 재현).
kernel::uint64_t* kAsTable(kernel::uint64_t physAddr) {
    return reinterpret_cast<kernel::uint64_t*>(kernel::kPhysToVirt(physAddr));
}

void kZeroTable(kernel::uint64_t* table) {
    for (int i = 0; i < 512; ++i) {
        table[i] = 0;
    }
}

// parentTable[index]가 다음 레벨 테이블을 가리키게 하고, 그 테이블의
// (identity-mapped) 포인터를 돌려준다 - 없으면 새로 만든다.
kernel::uint64_t* kGetOrCreateNextLevel(kernel::uint64_t* parentTable, kernel::uint32_t index, kernel::uint64_t flags) {
    if (parentTable[index] & kernel::PAGE_PRESENT) {
        parentTable[index] |= flags;  // 기존 권한에 이번 요청 권한을 더해준다(예: USER 승격)
        return kAsTable(parentTable[index] & kAddrMask);
    }
    const kernel::uint64_t newTablePhys = kernel::PageFrameAllocator::allocPage();
    kernel::uint64_t* newTable = kAsTable(newTablePhys);
    kZeroTable(newTable);
    parentTable[index] = newTablePhys | kernel::PAGE_PRESENT | kernel::PAGE_WRITABLE | flags;
    return newTable;
}

// [PL-57CF86EF 분할] pd[pdIndex]가 2M PS 엔트리면, 그 물리주소/플래그를
// 그대로 이어받는 새 PT(512개 4K 엔트리)로 되돌린다 - 이미 4K PT를
// 가리키고 있거나(PS 아님) 아예 비어 있으면 아무 일도 안 한다. 이
// 함수 뒤에는 항상 "pd[pdIndex]가 present라면 PT를 가리키는 일반
// 엔트리"임을 가정할 수 있다. TLB 무효화는 호출부가 실제로 건드릴
// 주소에 대해 하는 kInvalidatePage 한 번으로 충분하다(SDM - invlpg는
// 그 주소를 담고 있던 대형 페이지 TLB 엔트리 전체를 무효화한다).
void kSplitTwoMegabyte(kernel::uint64_t* pd, kernel::uint32_t pdIndex) {
    const kernel::uint64_t entry = pd[pdIndex];
    if (!(entry & kernel::PAGE_PRESENT) || !(entry & kPageSizeBit)) {
        return;
    }
    const kernel::uint64_t baseAddr = entry & kAddrMask;
    const kernel::uint64_t leafFlags = entry & kLeafFlagsMask;

    const kernel::uint64_t newTablePhys = kernel::PageFrameAllocator::allocPage();
    kernel::uint64_t* newTable = kAsTable(newTablePhys);
    for (kernel::uint32_t i = 0; i < kEntriesPerTable; ++i) {
        newTable[i] = (baseAddr + static_cast<kernel::uint64_t>(i) * kPageSize4K) | leafFlags;
    }
    pd[pdIndex] = newTablePhys | kernel::PAGE_PRESENT | kernel::PAGE_WRITABLE | (leafFlags & kernel::PAGE_USER);
}

kernel::uint32_t kPml4Index(kernel::uint64_t virtualAddr) { return (virtualAddr >> 39) & 0x1FF; }
kernel::uint32_t kPdptIndex(kernel::uint64_t virtualAddr) { return (virtualAddr >> 30) & 0x1FF; }
kernel::uint32_t kPdIndex(kernel::uint64_t virtualAddr) { return (virtualAddr >> 21) & 0x1FF; }
kernel::uint32_t kPtIndex(kernel::uint64_t virtualAddr) { return (virtualAddr >> 12) & 0x1FF; }

void kInvalidatePage(kernel::uint64_t virtualAddr) {
    asm volatile("invlpg (%0)" : : "r"(virtualAddr) : "memory");
}

}  // namespace

namespace kernel {

void Paging::init(uint64_t maxPhysAddr) {
    uint64_t* pml4 = kLowIdentityTable(kCurrentPml4Phys());
    auto* pdpt = reinterpret_cast<uint64_t*>(&gDirectMapPdptStorage[0]);
    kZeroTable(pdpt);

    uint64_t gibPages = (maxPhysAddr + kPageSize1G - 1) / kPageSize1G;
    if (gibPages < kMinDirectMapGib) {
        gibPages = kMinDirectMapGib;
    }
    if (gibPages > kMaxDirectMapGib) {
        gibPages = kMaxDirectMapGib;
    }

    for (uint32_t i = 0; i < gibPages; ++i) {  // 1GiB 페이지 gibPages개
        pdpt[i] = (static_cast<uint64_t>(i) * kPageSize1G) | PAGE_PRESENT | PAGE_WRITABLE | kPageSizeBit;
    }
    gDirectMapLimit = gibPages * kPageSize1G;

    // gDirectMapPdptStorage는 커널 이미지(higher-half) 안의 정적
    // 배열이라 그 "주소"는 이미 가상주소다 - PDPT 엔트리에는 물리
    // 주소가 필요하므로 KERNEL_VMA만큼(=kernel_phys_start와의 오프셋)
    // 빼서 되돌린다.
    const uint64_t pdptVirt = reinterpret_cast<uint64_t>(pdpt);
    constexpr uint64_t kKernelVma = 0xFFFFFFFF80000000UL;
    const uint64_t pdptPhys = pdptVirt - kKernelVma;

    const uint32_t pml4Index = kPml4Index(kDirectMapBase);
    pml4[pml4Index] = pdptPhys | PAGE_PRESENT | PAGE_WRITABLE;
}

void Paging::mapPage(uint64_t virtualAddr, uint64_t physicalAddr, uint64_t flags, uint64_t pml4Phys) {
    virtualAddr &= ~(kPageSize4K - 1);
    physicalAddr &= ~(kPageSize4K - 1);
    if (pml4Phys == 0) {
        pml4Phys = kCurrentPml4Phys();
    }

    uint64_t* pml4 = kAsTable(pml4Phys);
    uint64_t* pdpt = kGetOrCreateNextLevel(pml4, kPml4Index(virtualAddr), flags & PAGE_USER);
    uint64_t* pd = kGetOrCreateNextLevel(pdpt, kPdptIndex(virtualAddr), flags & PAGE_USER);
    // PL-57CF86EF 분할 - 이 4K 슬롯이 이미 2M 페이지에 속해 있으면
    // 아래서 그 엔트리를 PT 포인터로 잘못 해석하기 전에 먼저 풀어준다.
    kSplitTwoMegabyte(pd, kPdIndex(virtualAddr));
    uint64_t* pt = kGetOrCreateNextLevel(pd, kPdIndex(virtualAddr), flags & PAGE_USER);

    pt[kPtIndex(virtualAddr)] = physicalAddr | PAGE_PRESENT | flags;
    // 지금 실행 중인 주소공간(현재 CR3)에 대한 변경일 때만 TLB를
    // 무효화한다 - pml4Phys가 아직 CR3에 설치되지 않은 다른 주소공간을
    // 가리키면 이 코어의 TLB엔 애초에 그 매핑이 캐싱돼 있을 수 없다
    // (invlpg는 항상 "지금 이 코어가 보고 있는 주소공간" 기준으로만
    // 의미가 있다).
    if (pml4Phys == kCurrentPml4Phys()) {
        kInvalidatePage(virtualAddr);
    }
}

void Paging::mapRange(uint64_t virtualAddr, uint64_t physicalAddr, uint64_t sizeBytes, uint64_t flags, uint64_t pml4Phys) {
    if (pml4Phys == 0) {
        pml4Phys = kCurrentPml4Phys();
    }
    virtualAddr &= ~(kPageSize4K - 1);
    physicalAddr &= ~(kPageSize4K - 1);
    sizeBytes = (sizeBytes + kPageSize4K - 1) & ~(kPageSize4K - 1);

    uint64_t mapped = 0;
    while (mapped < sizeBytes) {
        const uint64_t va = virtualAddr + mapped;
        const uint64_t pa = physicalAddr + mapped;
        const uint64_t remaining = sizeBytes - mapped;

        if (kIsAligned(va, kPageSize2M) && kIsAligned(pa, kPageSize2M) && remaining >= kPageSize2M) {
            uint64_t* pml4 = kAsTable(pml4Phys);
            uint64_t* pdpt = kGetOrCreateNextLevel(pml4, kPml4Index(va), flags & PAGE_USER);
            uint64_t* pd = kGetOrCreateNextLevel(pdpt, kPdptIndex(va), flags & PAGE_USER);
            const uint32_t pdIndex = kPdIndex(va);
            if (!(pd[pdIndex] & PAGE_PRESENT)) {
                // [PL-57CF86EF 병합 경로 (a)] 이 2M 슬롯이 완전히
                // 비어 있을 때만 즉시 대형 페이지로 매핑한다 - 이미
                // 분가 있으면(2M이든 부분적으로 채워진 4K PT든) 기존
                // 매핑을 잃어버리지 않도록 안전하게 4K 경로로 물러난다
                // (아래 else 분기 없이 그냥 밑으로 흘러 mapPage() 호출).
                pd[pdIndex] = pa | PAGE_PRESENT | kPageSizeBit | flags;
                if (pml4Phys == kCurrentPml4Phys()) {
                    kInvalidatePage(va);
                }
                mapped += kPageSize2M;
                continue;
            }
        }
        mapPage(va, pa, flags, pml4Phys);
        mapped += kPageSize4K;
    }
}

bool Paging::mergeRange(uint64_t virtualAddr, uint64_t sizeBytes, uint64_t pml4Phys) {
    if (pml4Phys == 0) {
        pml4Phys = kCurrentPml4Phys();
    }
    const uint64_t start = virtualAddr & ~(kPageSize2M - 1);
    const uint64_t end = (virtualAddr + sizeBytes + kPageSize2M - 1) & ~(kPageSize2M - 1);

    bool mergedAny = false;
    uint64_t* pml4 = kAsTable(pml4Phys);
    for (uint64_t va = start; va < end; va += kPageSize2M) {
        if (!(pml4[kPml4Index(va)] & PAGE_PRESENT)) {
            continue;
        }
        uint64_t* pdpt = kAsTable(pml4[kPml4Index(va)] & kAddrMask);
        if (!(pdpt[kPdptIndex(va)] & PAGE_PRESENT)) {
            continue;
        }
        uint64_t* pd = kAsTable(pdpt[kPdptIndex(va)] & kAddrMask);
        const uint32_t pdIndex = kPdIndex(va);
        const uint64_t pdEntry = pd[pdIndex];
        if (!(pdEntry & PAGE_PRESENT) || (pdEntry & kPageSizeBit)) {
            continue;  // 이미 2M이거나 아예 비어 있음 - 병합할 게 없음
        }

        uint64_t* pt = kAsTable(pdEntry & kAddrMask);
        if (!(pt[0] & PAGE_PRESENT)) {
            continue;
        }
        const uint64_t baseAddr = pt[0] & kAddrMask;
        const uint64_t leafFlags = pt[0] & kLeafFlagsMask;
        if (!kIsAligned(baseAddr, kPageSize2M)) {
            continue;  // 2M PS 엔트리는 하드웨어상 물리주소도 2M 정렬이어야 한다
        }

        bool qualifies = true;
        for (uint32_t i = 1; i < kEntriesPerTable && qualifies; ++i) {
            const uint64_t entry = pt[i];
            const bool contiguous = (entry & kAddrMask) == baseAddr + static_cast<uint64_t>(i) * kPageSize4K;
            if (!(entry & PAGE_PRESENT) || !contiguous || (entry & kLeafFlagsMask) != leafFlags) {
                qualifies = false;
            }
        }
        if (!qualifies) {
            continue;  // 병합 불변 조건 불만족 - 재배치(경로 b)는 이번 범위 밖, 그대로 둘
        }

        const uint64_t ptPhys = pdEntry & kAddrMask;
        pd[pdIndex] = baseAddr | kPageSizeBit | leafFlags;
        if (pml4Phys == kCurrentPml4Phys()) {
            kInvalidatePage(va);
        }
        PageFrameAllocator::freePage(ptPhys);
        mergedAny = true;
    }
    return mergedAny;
}

bool Paging::handlePageFault(uint64_t faultAddr, uint64_t errorCode) {
    constexpr uint64_t kErrorCodePresentBit = 1UL << 0;
    if (errorCode & kErrorCodePresentBit) {
        return false;  // 이미 매핑된 페이지에 대한 권한 위반 - 조용히 넘기지 않는다
    }
    if (faultAddr < kLazyZoneBase || faultAddr >= kLazyZoneBase + kLazyZoneSize) {
        return false;  // 지연 매핑 구역 밖 - 진짜 잘못된 접근
    }

    const uint64_t phys = PageFrameAllocator::allocPage();
    if (!phys) {
        return false;  // OOM - 매핑해줄 방법이 없으니 그대로 패닉시킨다
    }

    mapPage(faultAddr & ~(kPageSize4K - 1), phys, PAGE_WRITABLE);
    return true;
}

void Paging::unmapPage(uint64_t virtualAddr, uint64_t pml4Phys) {
    virtualAddr &= ~(kPageSize4K - 1);
    if (pml4Phys == 0) {
        pml4Phys = kCurrentPml4Phys();
    }

    uint64_t* pml4 = kAsTable(pml4Phys);
    if (!(pml4[kPml4Index(virtualAddr)] & PAGE_PRESENT)) {
        return;
    }
    uint64_t* pdpt = kAsTable(pml4[kPml4Index(virtualAddr)] & kAddrMask);
    if (!(pdpt[kPdptIndex(virtualAddr)] & PAGE_PRESENT)) {
        return;
    }
    uint64_t* pd = kAsTable(pdpt[kPdptIndex(virtualAddr)] & kAddrMask);
    if (!(pd[kPdIndex(virtualAddr)] & PAGE_PRESENT)) {
        return;
    }
    // PL-57CF86EF 분할 - 지우려는 4K 페이지가 2M 페이지의 일부라면,
    // 나머지 511개는 그대로 살려 두고 이 한 페이지만 지워야 하므로
    // 먼저 진짜 4K PT로 되돌린다.
    kSplitTwoMegabyte(pd, kPdIndex(virtualAddr));
    uint64_t* pt = kAsTable(pd[kPdIndex(virtualAddr)] & kAddrMask);
    pt[kPtIndex(virtualAddr)] = 0;
    if (pml4Phys == kCurrentPml4Phys()) {
        kInvalidatePage(virtualAddr);
    }
}

uint64_t Paging::translatePage(uint64_t virtualAddr, uint64_t pml4Phys) {
    virtualAddr &= ~(kPageSize4K - 1);
    if (pml4Phys == 0) {
        pml4Phys = kCurrentPml4Phys();
    }

    uint64_t* pml4 = kAsTable(pml4Phys);
    if (!(pml4[kPml4Index(virtualAddr)] & PAGE_PRESENT)) {
        return 0;
    }
    uint64_t* pdpt = kAsTable(pml4[kPml4Index(virtualAddr)] & kAddrMask);
    if (!(pdpt[kPdptIndex(virtualAddr)] & PAGE_PRESENT)) {
        return 0;
    }
    uint64_t* pd = kAsTable(pdpt[kPdptIndex(virtualAddr)] & kAddrMask);
    const uint64_t pdEntry = pd[kPdIndex(virtualAddr)];
    if (!(pdEntry & PAGE_PRESENT)) {
        return 0;
    }
    if (pdEntry & kPageSizeBit) {
        // PL-57CF86EF - 순수 조회라 굳이 분할하지 않고, 2M 엔트리
        // 안에서의 오프셋만 계산해 답한다(페이지 테이블을 안 건드림).
        const uint64_t offsetWithin2M = virtualAddr & (kPageSize2M - 1);
        return (pdEntry & kAddrMask) + offsetWithin2M;
    }
    uint64_t* pt = kAsTable(pdEntry & kAddrMask);
    if (!(pt[kPtIndex(virtualAddr)] & PAGE_PRESENT)) {
        return 0;
    }
    return pt[kPtIndex(virtualAddr)] & kAddrMask;
}

uint64_t Paging::currentPml4Phys() {
    return kCurrentPml4Phys();
}

uint64_t Paging::directMapLimit() {
    return gDirectMapLimit;
}

uint64_t Paging::createAddressSpace() {
    const uint64_t newPml4Phys = PageFrameAllocator::allocPage();
    if (!newPml4Phys) {
        return 0;
    }
    uint64_t* newPml4 = kAsTable(newPml4Phys);
    kZeroTable(newPml4);

    const uint64_t* sourcePml4 = kAsTable(kCurrentPml4Phys());
    for (uint32_t i = kHigherHalfPml4Start; i < kPml4EntryCount; ++i) {
        newPml4[i] = sourcePml4[i];
    }
    return newPml4Phys;
}

void Paging::destroyAddressSpace(uint64_t pml4Phys) {
    PageFrameAllocator::freePage(pml4Phys);
}

}  // namespace kernel
