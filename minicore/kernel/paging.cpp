#include "paging.h"

#include "libkenv/types.h"
#include "page_frame_allocator.h"

namespace {

constexpr kernel::uint64_t kPageSize4K = 0x1000;
constexpr kernel::uint64_t kPageSize1G = 0x40000000UL;
constexpr kernel::uint64_t kAddrMask = 0x000FFFFFFFFFF000UL;  // 엔트리에서 플래그 비트 뺀 물리주소
constexpr kernel::uint64_t kPageSizeBit = 1UL << 7;           // PS(PDPT/PD 레벨 대형 페이지)

// 커널 higher-half의 시작 PML4 인덱스(Paging::createAddressSpace) -
// kDirectMapBase(0xFFFF800000000000)가 정확히 이 경계다(canonical
// 주소의 부호 확장 경계, bit 47). 인덱스 256~511(총 256개)을 통째로
// 복사하면 direct map/지연 매핑 구역/커널 이미지 자신(kKernelVma
// 근방)까지 전부 한 번에 커버된다 - 이 셋의 정확한 하위 배치를
// 개별적으로 알 필요가 없다.
constexpr kernel::uint32_t kHigherHalfPml4Start = 256;
constexpr kernel::uint32_t kPml4EntryCount = 512;

// direct map용 PDPT 하나만 정적으로 예약한다(컴파일 타임 .bss, 커널
// 자신의 higher-half 이미지 안이라 이미 매핑돼 있다 - PageFrameAllocator
// 초기화 전에도 안전하게 쓸 수 있다). 첫 4GiB만 1GiB 페이지로 덮는다.
alignas(4096) kernel::uint8_t gDirectMapPdptStorage[4096];

kernel::uint64_t kCurrentPml4Phys() {
    kernel::uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & kAddrMask;
}

// Paging::init() 자신이 direct map을 만들기 전에 딱 한 번(자기 자신의
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

kernel::uint32_t kPml4Index(kernel::uint64_t virtualAddr) { return (virtualAddr >> 39) & 0x1FF; }
kernel::uint32_t kPdptIndex(kernel::uint64_t virtualAddr) { return (virtualAddr >> 30) & 0x1FF; }
kernel::uint32_t kPdIndex(kernel::uint64_t virtualAddr) { return (virtualAddr >> 21) & 0x1FF; }
kernel::uint32_t kPtIndex(kernel::uint64_t virtualAddr) { return (virtualAddr >> 12) & 0x1FF; }

void kInvalidatePage(kernel::uint64_t virtualAddr) {
    asm volatile("invlpg (%0)" : : "r"(virtualAddr) : "memory");
}

}  // namespace

namespace kernel {

void Paging::init() {
    uint64_t* pml4 = kLowIdentityTable(kCurrentPml4Phys());
    auto* pdpt = reinterpret_cast<uint64_t*>(&gDirectMapPdptStorage[0]);
    kZeroTable(pdpt);

    for (uint32_t i = 0; i < 4; ++i) {  // 0~3GiB, 1GiB 페이지 4개
        pdpt[i] = (static_cast<uint64_t>(i) * kPageSize1G) | PAGE_PRESENT | PAGE_WRITABLE | kPageSizeBit;
    }

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
    uint64_t* pt = kGetOrCreateNextLevel(pd, kPdIndex(virtualAddr), flags & PAGE_USER);

    pt[kPtIndex(virtualAddr)] = physicalAddr | PAGE_PRESENT | flags;
    // 지금 실행 중인 주소공간(현재 CR3)에 대한 변경일 때만 TLB를
    // 무효화한다 - pml4Phys가 아직 CR3에 설치되지 않은 다른 주소공간을
    // 가리키면 이 코어의 TLB엔 애초에 그 매핑이 캐시돼 있을 수 없다
    // (invlpg는 항상 "지금 이 코어가 보고 있는 주소공간" 기준으로만
    // 의미가 있다).
    if (pml4Phys == kCurrentPml4Phys()) {
        kInvalidatePage(virtualAddr);
    }
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
    uint64_t* pt = kAsTable(pd[kPdIndex(virtualAddr)] & kAddrMask);
    pt[kPtIndex(virtualAddr)] = 0;
    if (pml4Phys == kCurrentPml4Phys()) {
        kInvalidatePage(virtualAddr);
    }
}

uint64_t Paging::currentPml4Phys() {
    return kCurrentPml4Phys();
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
