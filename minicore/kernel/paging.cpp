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

unsigned long* kAsTable(unsigned long physAddr) {
    // PageFrameAllocator가 주는 프레임은 항상 저지대 1GiB 안(identity
    // map)이라 물리 주소를 그대로 포인터로 쓸 수 있다. direct map이
    // 준비된 뒤에는 kPhysToVirt를 거치는 편이 더 일반적이지만, 부트
    // 스트랩 단계(Paging::kInit 자신)에서는 아직 direct map이 없으므로
    // 여기서는 항상 identity 가정으로 접근한다 - 이 가정이 깨지는
    // 유일한 경우는 PageFrameAllocator 관리 범위가 1GiB를 넘어설 때뿐
    // 이다(그때는 이 함수도 같이 고쳐야 한다).
    return reinterpret_cast<unsigned long*>(physAddr);
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
    const unsigned long newTablePhys = kernel::PageFrameAllocator::kAllocPage();
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

void Paging::kInit() {
    unsigned long* pml4 = kAsTable(kCurrentPml4Phys());
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

void Paging::kMapPage(unsigned long virtualAddr, unsigned long physicalAddr, unsigned long flags) {
    virtualAddr &= ~(kPageSize4K - 1);
    physicalAddr &= ~(kPageSize4K - 1);

    unsigned long* pml4 = kAsTable(kCurrentPml4Phys());
    unsigned long* pdpt = kGetOrCreateNextLevel(pml4, kPml4Index(virtualAddr), flags & PAGE_USER);
    unsigned long* pd = kGetOrCreateNextLevel(pdpt, kPdptIndex(virtualAddr), flags & PAGE_USER);
    unsigned long* pt = kGetOrCreateNextLevel(pd, kPdIndex(virtualAddr), flags & PAGE_USER);

    pt[kPtIndex(virtualAddr)] = physicalAddr | PAGE_PRESENT | flags;
    kInvalidatePage(virtualAddr);
}

void Paging::kUnmapPage(unsigned long virtualAddr) {
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
