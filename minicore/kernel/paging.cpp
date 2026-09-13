#include "paging.h"

#include "page_frame_allocator.h"

namespace {

constexpr unsigned long kPageSize4K = 0x1000;
constexpr unsigned long kPageSize1G = 0x40000000UL;
constexpr unsigned long kAddrMask = 0x000FFFFFFFFFF000UL;  // 엔트리에서 플래그 비트 뺀 물리주소
constexpr unsigned long kPageSizeBit = 1UL << 7;           // PS(PDPT/PD 레벨 대형 페이지)

// direct map용 PDPT 하나만 정적으로 예약한다(컴파일 타임 .bss, 커널
// 자신의 higher-half 이미지 안이라 이미 매핑돼 있다 - PageFrameAllocator
// 초기화 전에도 안전하게 쓸 수 있다). 첫 4GiB만 1GiB 페이지로 덮는다.
alignas(4096) unsigned char gDirectMapPdptStorage[4096];

unsigned long kCurrentPml4Phys() {
    unsigned long cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & kAddrMask;
}

// Paging::init() 자신이 direct map을 만들기 전에 딱 한 번(자기 자신의
// PML4를 읽으려고) 쓴다 - 그 시점엔 kPhysToVirt를 아직 못 쓴다(direct
// map이 없으니까). boot.S가 PML4를 항상 저지대 identity map 구간에
// 두기 때문에 안전하다. **이 함수는 Paging::init() 밖에서 쓰면 안
// 된다** - PageFrameAllocator가 그 뒤로는 1GiB 밖 프레임도 내주므로
// (PL-99562483) identity 가정이 깨진다.
unsigned long* kLowIdentityTable(unsigned long physAddr) {
    return reinterpret_cast<unsigned long*>(physAddr);
}

// direct map(0~4GiB, Paging::init() 이후 항상 존재)을 거쳐 임의 물리
// 프레임을 가리키는 포인터를 얻는다 - PageFrameAllocator가 내주는
// 프레임이 이제 1GiB를 넘어설 수 있어(PL-99562483, 2026-09-14) 더는
// identity 캐스팅을 쓰면 안 된다(실측으로 페이지폴트 걸림 - -m 2048
// 환경에서 재현).
unsigned long* kAsTable(unsigned long physAddr) {
    return reinterpret_cast<unsigned long*>(kernel::kPhysToVirt(physAddr));
}

void kZeroTable(unsigned long* table) {
    for (int i = 0; i < 512; ++i) {
        table[i] = 0;
    }
}

// parentTable[index]가 다음 레벨 테이블을 가리키게 하고, 그 테이블의
// (identity-mapped) 포인터를 돌려준다 - 없으면 새로 만든다.
unsigned long* kGetOrCreateNextLevel(unsigned long* parentTable, unsigned int index, unsigned long flags) {
    if (parentTable[index] & kernel::PAGE_PRESENT) {
        parentTable[index] |= flags;  // 기존 권한에 이번 요청 권한을 더해준다(예: USER 승격)
        return kAsTable(parentTable[index] & kAddrMask);
    }
    const unsigned long newTablePhys = kernel::PageFrameAllocator::allocPage();
    unsigned long* newTable = kAsTable(newTablePhys);
    kZeroTable(newTable);
    parentTable[index] = newTablePhys | kernel::PAGE_PRESENT | kernel::PAGE_WRITABLE | flags;
    return newTable;
}

unsigned int kPml4Index(unsigned long virtualAddr) { return (virtualAddr >> 39) & 0x1FF; }
unsigned int kPdptIndex(unsigned long virtualAddr) { return (virtualAddr >> 30) & 0x1FF; }
unsigned int kPdIndex(unsigned long virtualAddr) { return (virtualAddr >> 21) & 0x1FF; }
unsigned int kPtIndex(unsigned long virtualAddr) { return (virtualAddr >> 12) & 0x1FF; }

void kInvalidatePage(unsigned long virtualAddr) {
    asm volatile("invlpg (%0)" : : "r"(virtualAddr) : "memory");
}

}  // namespace

namespace kernel {

void Paging::init() {
    unsigned long* pml4 = kLowIdentityTable(kCurrentPml4Phys());
    auto* pdpt = reinterpret_cast<unsigned long*>(&gDirectMapPdptStorage[0]);
    kZeroTable(pdpt);

    for (unsigned int i = 0; i < 4; ++i) {  // 0~3GiB, 1GiB 페이지 4개
        pdpt[i] = (static_cast<unsigned long>(i) * kPageSize1G) | PAGE_PRESENT | PAGE_WRITABLE | kPageSizeBit;
    }

    // gDirectMapPdptStorage는 커널 이미지(higher-half) 안의 정적
    // 배열이라 그 "주소"는 이미 가상주소다 - PDPT 엔트리에는 물리
    // 주소가 필요하므로 KERNEL_VMA만큼(=kernel_phys_start와의 오프셋)
    // 빼서 되돌린다.
    const unsigned long pdptVirt = reinterpret_cast<unsigned long>(pdpt);
    constexpr unsigned long kKernelVma = 0xFFFFFFFF80000000UL;
    const unsigned long pdptPhys = pdptVirt - kKernelVma;

    const unsigned int pml4Index = kPml4Index(kDirectMapBase);
    pml4[pml4Index] = pdptPhys | PAGE_PRESENT | PAGE_WRITABLE;
}

void Paging::mapPage(unsigned long virtualAddr, unsigned long physicalAddr, unsigned long flags) {
    virtualAddr &= ~(kPageSize4K - 1);
    physicalAddr &= ~(kPageSize4K - 1);

    unsigned long* pml4 = kAsTable(kCurrentPml4Phys());
    unsigned long* pdpt = kGetOrCreateNextLevel(pml4, kPml4Index(virtualAddr), flags & PAGE_USER);
    unsigned long* pd = kGetOrCreateNextLevel(pdpt, kPdptIndex(virtualAddr), flags & PAGE_USER);
    unsigned long* pt = kGetOrCreateNextLevel(pd, kPdIndex(virtualAddr), flags & PAGE_USER);

    pt[kPtIndex(virtualAddr)] = physicalAddr | PAGE_PRESENT | flags;
    kInvalidatePage(virtualAddr);
}

bool Paging::handlePageFault(unsigned long faultAddr, unsigned long errorCode) {
    constexpr unsigned long kErrorCodePresentBit = 1UL << 0;
    if (errorCode & kErrorCodePresentBit) {
        return false;  // 이미 매핑된 페이지에 대한 권한 위반 - 조용히 넘기지 않는다
    }
    if (faultAddr < kLazyZoneBase || faultAddr >= kLazyZoneBase + kLazyZoneSize) {
        return false;  // 지연 매핑 구역 밖 - 진짜 잘못된 접근
    }

    const unsigned long phys = PageFrameAllocator::allocPage();
    if (!phys) {
        return false;  // OOM - 매핑해줄 방법이 없으니 그대로 패닉시킨다
    }

    mapPage(faultAddr & ~(kPageSize4K - 1), phys, PAGE_WRITABLE);
    return true;
}

void Paging::unmapPage(unsigned long virtualAddr) {
    virtualAddr &= ~(kPageSize4K - 1);

    unsigned long* pml4 = kAsTable(kCurrentPml4Phys());
    if (!(pml4[kPml4Index(virtualAddr)] & PAGE_PRESENT)) {
        return;
    }
    unsigned long* pdpt = kAsTable(pml4[kPml4Index(virtualAddr)] & kAddrMask);
    if (!(pdpt[kPdptIndex(virtualAddr)] & PAGE_PRESENT)) {
        return;
    }
    unsigned long* pd = kAsTable(pdpt[kPdptIndex(virtualAddr)] & kAddrMask);
    if (!(pd[kPdIndex(virtualAddr)] & PAGE_PRESENT)) {
        return;
    }
    unsigned long* pt = kAsTable(pd[kPdIndex(virtualAddr)] & kAddrMask);
    pt[kPtIndex(virtualAddr)] = 0;
    kInvalidatePage(virtualAddr);
}

}  // namespace kernel
