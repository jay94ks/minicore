#include "paging.h"

#include "async_task.h"
#include "block_device.h"
#include "libkenv/chunked_list.h"
#include "libkenv/mem.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"
#include "libswapfs/swapfs.h"
#include "swap_backend.h"
#include "page_frame_allocator.h"
#include "process.h"
#include "scheduler.h"
#include "tlb_shootdown.h"
#include "x86_64/msr.h"

namespace {

constexpr kernel::uint64_t kPageSize4K = 0x1000;
constexpr kernel::uint64_t kPageSize2M = 0x200000UL;
constexpr kernel::uint64_t kPageSize1G = 0x40000000UL;
constexpr kernel::uint64_t kAddrMask = 0x000FFFFFFFFFF000UL;  // 엔트리에서 플래그 비트를 제외한 물리주소
constexpr kernel::uint64_t kPageSizeBit = 1UL << 7;           // PS(PDPT/PD 레벨 대형 페이지)
constexpr kernel::uint32_t kEntriesPerTable = 512;            // x86_64 4단계 페이징의 모든 테이블(PML4/PDPT/PD/PT)은 512엔트리 고정

// PL-57CF86EF 병합 불변 조건이 실제로 비교하는 leaf 엔트리 플래그
// 집합 - PRESENT를 포함해 두면 분할/병합 시 이 값을 그대로 leaf
// 엔트리에 다시 써넣는 것만으로 PRESENT 비트까지 함께 복원된다.
constexpr kernel::uint64_t kLeafFlagsMask =
    kernel::PAGE_PRESENT | kernel::PAGE_WRITABLE | kernel::PAGE_USER | kernel::PAGE_CACHE_DISABLE;

// [신규, 2026-09-18, SP-8D206F11 §2.2] IA32_PAT MSR 번호 + 인덱스4를
// WC로 재정의한 값 - 인덱스 0~3/5~7은 Intel SDM의 하드웨어 리셋
// 기본값(Table 11-11)을 그대로 유지한다(각 바이트는 그 인덱스의
// 메모리 타입 인코딩 - 06h=WB, 04h=WT, 07h=UC-, 00h=UC, 01h=WC).
// 이름 붙은 인코딩 상수로 조립해 매직 넘버를 피한다(Paging::
// initPatForThisCore() 문서 주석의 표와 정확히 대응).
constexpr kernel::uint32_t kMsrPat = 0x277;
constexpr kernel::uint64_t kPatEncodingWb = 0x06;
constexpr kernel::uint64_t kPatEncodingWt = 0x04;
constexpr kernel::uint64_t kPatEncodingUcMinus = 0x07;
constexpr kernel::uint64_t kPatEncodingUc = 0x00;
constexpr kernel::uint64_t kPatEncodingWc = 0x01;
constexpr kernel::uint64_t kPatValueWithIndex4Wc =
    (kPatEncodingUc << 56) |        // 인덱스7 - 기존 그대로(1~3의 미러)
    (kPatEncodingUcMinus << 48) |   // 인덱스6
    (kPatEncodingWt << 40) |        // 인덱스5
    (kPatEncodingWc << 32) |        // 인덱스4 - [신규] Write-Combining
    (kPatEncodingUc << 24) |        // 인덱스3 - 기존 그대로(진짜 UC)
    (kPatEncodingUcMinus << 16) |   // 인덱스2 - 기존 그대로(UC-, PAGE_CACHE_DISABLE 단독 사용처가 여기)
    (kPatEncodingWt << 8) |         // 인덱스1 - 기존 그대로
    (kPatEncodingWb << 0);          // 인덱스0 - 기존 그대로

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

// Paging::init() 자신이 direct map을 만들기 전에 정확히 한 번(자기 자신의
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
//
// [수정, 2026-09-17, PN-90BD044E/DC-FB38F86F(A) 채택, 설계자 답변]
// 원래 이 함수는 완전히 무잠금이라 두 코어가 동시에 같은 아직-없는
// parentTable[index]를 통과하면 각자 다른 테이블을 만들어 나중에 쓴
// 쪽이 이기는 TOCTOU 경쟁이 있었다(먼저 쓴 쪽이 채운 하위 매핑은
// 도달 불가능한 고아가 됨, 상세 근거는 PN-90BD044E) - 지금은 항상
// 호출자(Paging::mapPage/mapRange/mergeRange 공개 진입점)가 미리
// 잡아 둔 주소공간별 락(또는 higher-half 전역 락)을 쥔 채로만
// 불린다는 것을 전제한다(이 함수 자신은 락을 잡지 않는다 - 이미
// 호출자가 잡고 있으므로 재진입 불가 Spinlock을 다시 잡으면
// 데드락이다).
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

// [신규, 2026-09-17, PN-90BD044E/DC-FB38F86F] 위 TOCTOU 경쟁을 막는
// 실제 락 두 종류 - 설계자가 (A) 주소공간별 전용 락을 채택하면서
// 원안이 이미 지적한 higher-half(여러 주소공간이 물리적으로 공유하는
// 커널 공용 PDPT/PD/PT 영역, Paging::createAddressSpace()의 엔트리
// 복사 방식 참고)용 별도 전역 락도 함께 필요하다는 트레이드오프를
// 그대로 유지했다. 최상위 PML4 인덱스 하나만 higher-half인지 확인하면
// 충분한 이유 - 그 아래 PDPT/PD/PT 세 단계는 higher-half PML4 엔트리
// 밑에서는 어느 주소공간을 거치든 항상 같은 물리 테이블을 가리키므로,
// 서로 다른 프로세스가 각자의 "주소공간별" 락을 잡아 봐야 그 공유
// 테이블에 대한 경쟁을 막지 못한다 - 반드시 이 전역 락 하나로만
// 통일해야 한다.
kernel::Spinlock gHigherHalfPagingLock;

// 주소공간(pml4Phys)별 전용 락 레지스트리 - Paging::createAddressSpace()
// 가 새 pml4Phys를 발급하는 시점에 항목을 등록하고 destroyAddressSpace()
// 가 반납할 때 제거한다. 락 자체(Spinlock)는 슬롯 안에 값으로 산다
// (별도 힙 할당 불필요) - ChunkedList<T,N>는 이 커널 전역에서 이미
// "성장 가능한 항목 레지스트리"에 쓰이는 확립된 패턴(Process::children/
// ResourceGroup::memberProcesses 등)이라 그대로 재사용했다.
// gAddressSpaceLockRegistryLock 자신은 삽입/삭제/조회(선형 탐색)
// 동안만 아주 짧게 잡힌다 - 실제 페이지 테이블 조작처럼 오래 걸리는
// 임계 구역은 조회로 얻은 그 항목의 Spinlock 쪽이 담당한다(레지스트리
// 락과 개별 주소공간 락은 절대 동시에 둘 다 오래 쥐지 않는다).
struct AddressSpaceLockEntry {
    kernel::uint64_t pml4Phys = 0;
    kernel::Spinlock lock;
};
kernel::ChunkedList<AddressSpaceLockEntry, 8> gAddressSpaceLocks;
kernel::Spinlock gAddressSpaceLockRegistryLock;

// 있으면 찾아 돌려주고, 없으면(정상 경로라면 항상 Paging::
// createAddressSpace()가 미리 등록해 뒀어야 하지만, 방어적으로) 그
// 자리에서 새로 등록한다 - 절대 "락을 못 찾았으니 무잠금으로 진행"
// 하지 않는다.
kernel::Spinlock* kFindOrCreateAddressSpaceLock(kernel::uint64_t pml4Phys) {
    kernel::SpinlockGuard guard(gAddressSpaceLockRegistryLock);
    auto* slot = gAddressSpaceLocks.find(
        [pml4Phys](const AddressSpaceLockEntry& e) { return e.pml4Phys == pml4Phys; });
    if (slot) {
        return &slot->value.lock;
    }
    gAddressSpaceLocks.ensureAllocator(&kernel::GenericSlabAllocator::alloc, &kernel::GenericSlabAllocator::free);
    AddressSpaceLockEntry entry;
    entry.pml4Phys = pml4Phys;
    auto* newSlot = gAddressSpaceLocks.insert(entry);
    return newSlot ? &newSlot->value.lock : nullptr;  // OOM - 호출부가 최후 방어를 책임진다
}

void kRemoveAddressSpaceLock(kernel::uint64_t pml4Phys) {
    kernel::SpinlockGuard guard(gAddressSpaceLockRegistryLock);
    auto* slot = gAddressSpaceLocks.find(
        [pml4Phys](const AddressSpaceLockEntry& e) { return e.pml4Phys == pml4Phys; });
    if (slot) {
        gAddressSpaceLocks.erase(slot);
    }
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

// [PN-2E6CB2D5] destroyAddressSpace()의 중간 테이블 재귀 반납 -
// depth==3(PDPT, 엔트리는 PD를 가리킴)/depth==2(PD, 엔트리는 PT를
// 가리키거나 PS 2M leaf일 수 있음)/depth==1(PT, 엔트리는 4K leaf
// 데이터 페이지 - 더 내려갈 테이블이 없음)만 쓰인다. leaf 데이터
// 페이지(PT 엔트리, PD의 PS 2M 엔트리) 자체는 호출부가 destroyAddressSpace
// 호출 전에 unmapPage로 이미 반납했어야 하는 대상이라 여기서는 절대
// 건드리지 않는다 - 오직 "테이블로 쓰인 프레임" 자신만 반납한다.
// PD의 PS 2M 엔트리는 (정상 경로라면 unmapPage의 kSplitTwoMegabyte가
// 이미 4K PT로 되돌려 놨겠지만) 방어적으로 건너뛴다 - 데이터 프레임을
// 테이블 프레임으로 착각해 반납하면 실행 중인 다른 매핑을 깨뜨린다.
void kFreeUserPageTablesRecursive(kernel::uint64_t tablePhys, kernel::uint32_t depth) {
    kernel::uint64_t* table = kAsTable(tablePhys);
    for (kernel::uint32_t i = 0; i < kEntriesPerTable; ++i) {
        const kernel::uint64_t entry = table[i];
        if (!(entry & kernel::PAGE_PRESENT)) {
            continue;
        }
        if (depth == 2 && (entry & kPageSizeBit)) {
            continue;  // PD의 2M leaf 데이터 엔트리 - 테이블이 아니다
        }
        if (depth > 1) {
            kFreeUserPageTablesRecursive(entry & kAddrMask, depth - 1);
        }
        // depth==1(PT)의 present 엔트리는 4K leaf 데이터 페이지 자체라
        // 더 내려갈 테이블이 없다 - 위 unmapPage 사전조건이 지켜졌다면
        // 애초에 여기까지 present로 남아있지 않아야 정상이다.
    }
    kernel::PageFrameAllocator::freePage(tablePhys);
}

kernel::uint32_t kPml4Index(kernel::uint64_t virtualAddr) { return (virtualAddr >> 39) & 0x1FF; }
kernel::uint32_t kPdptIndex(kernel::uint64_t virtualAddr) { return (virtualAddr >> 30) & 0x1FF; }
kernel::uint32_t kPdIndex(kernel::uint64_t virtualAddr) { return (virtualAddr >> 21) & 0x1FF; }
kernel::uint32_t kPtIndex(kernel::uint64_t virtualAddr) { return (virtualAddr >> 12) & 0x1FF; }

void kInvalidatePage(kernel::uint64_t virtualAddr) {
    asm volatile("invlpg (%0)" : : "r"(virtualAddr) : "memory");
}

// virtualAddr의 최상위 PML4 인덱스만으로 어느 락을 잡을지 고른다 -
// 호출부(Paging::mapPage/mapRange/mergeRange)가 실제 트리 조작 전에
// 정확히 한 번만 부른다. 레지스트리 OOM(극히 드묾)일 때조차 절대
// nullptr을 반환하지 않고 higher-half 전역 락으로 물러난다 - "락을
// 아예 못 잡는" 경로를 남겨 두지 않기 위한 최후 방어.
kernel::Spinlock* kLockForAddressSpaceOp(kernel::uint64_t virtualAddr, kernel::uint64_t pml4Phys) {
    if (kPml4Index(virtualAddr) >= kHigherHalfPml4Start) {
        return &gHigherHalfPagingLock;
    }
    kernel::Spinlock* lock = kFindOrCreateAddressSpaceLock(pml4Phys);
    return lock ? lock : &gHigherHalfPagingLock;
}

// [신규, 2026-09-23, PN-4859FDE9] testAndClearAccessed/kHandleCowWriteFault가
// 각자 반복해 온 PML4->PDPT->PD->PT 순회를, PTE 값 자체를 읽고/고치는
// 여러 단계를 하나의 임계구역으로 묶어야 하는 새 함수들(아래
// markReclaimInProgress 등)을 위해 한 번만 뽑아 둔다 - 호출자가 이미
// kLockForAddressSpaceOp()로 얻은 락을 쥐고 있어야 한다(이 함수 자신은
// 잠그지 않음). 중간 테이블이 없거나(진짜 미매핑) PD 엔트리가 2MiB
// 대형 페이지면 nullptr - 이 스캔/회수 계열 전부의 공통 전제(anonymous
// rmap 매핑은 항상 4KiB 단일 페이지, PageFrameAllocator::retain() 문서
// 주석과 동일)이다.
kernel::uint64_t* kFindLeafPteLocked(kernel::uint64_t virtualAddr, kernel::uint64_t pml4Phys) {
    kernel::uint64_t* pml4 = kAsTable(pml4Phys);
    if (!(pml4[kPml4Index(virtualAddr)] & kernel::PAGE_PRESENT)) {
        return nullptr;
    }
    kernel::uint64_t* pdpt = kAsTable(pml4[kPml4Index(virtualAddr)] & kAddrMask);
    if (!(pdpt[kPdptIndex(virtualAddr)] & kernel::PAGE_PRESENT)) {
        return nullptr;
    }
    kernel::uint64_t* pd = kAsTable(pdpt[kPdptIndex(virtualAddr)] & kAddrMask);
    const kernel::uint64_t pdEntry = pd[kPdIndex(virtualAddr)];
    if (!(pdEntry & kernel::PAGE_PRESENT) || (pdEntry & kPageSizeBit)) {
        return nullptr;
    }
    kernel::uint64_t* pt = kAsTable(pdEntry & kAddrMask);
    return &pt[kPtIndex(virtualAddr)];
}

// [신규, 2026-09-16, SP-6BEAE0C1 §2/§11, PN-543C0CE9 착수 6번째 증분]
// `PAGE_COW` 쓰기 폴트 처리 - 항상 **지금 실행 중인(CR3) 주소공간**
// 기준으로만 동작한다(#PF는 그 폴트를 일으킨 코드가 실제로 실행되던
// 바로 그 주소공간에서만 발생하므로, mapPage 등 다른 API처럼 임의
// pml4Phys를 받을 이유가 없다). faultAddr가 실제로 present+COW+
// non-writable 4K leaf를 가리킬 때만 true - 그 외(테이블 경로가
// 끊겨 있음, 2M 대형 페이지 - v1 COW는 4K 리프만 대상, COW 비트
// 없음)는 false(호출부 handlePageFault가 그대로 패닉시킨다).
bool kHandleCowWriteFault(kernel::uint64_t faultAddr) {
    const kernel::uint64_t va = faultAddr & ~(kPageSize4K - 1);
    const kernel::uint64_t pml4Phys = kCurrentPml4Phys();

    kernel::uint64_t* pml4 = kAsTable(pml4Phys);
    if (!(pml4[kPml4Index(va)] & kernel::PAGE_PRESENT)) {
        return false;
    }
    kernel::uint64_t* pdpt = kAsTable(pml4[kPml4Index(va)] & kAddrMask);
    if (!(pdpt[kPdptIndex(va)] & kernel::PAGE_PRESENT)) {
        return false;
    }
    kernel::uint64_t* pd = kAsTable(pdpt[kPdptIndex(va)] & kAddrMask);
    const kernel::uint64_t pdEntry = pd[kPdIndex(va)];
    if (!(pdEntry & kernel::PAGE_PRESENT) || (pdEntry & kPageSizeBit)) {
        return false;  // 없거나 2M 대형 페이지(v1 COW는 4K 리프 전제) - COW 대상 아님
    }
    kernel::uint64_t* pt = kAsTable(pdEntry & kAddrMask);
    const kernel::uint32_t ptIndex = kPtIndex(va);
    const kernel::uint64_t ptEntry = pt[ptIndex];
    if (!(ptEntry & kernel::PAGE_PRESENT) || !(ptEntry & kernel::PAGE_COW)) {
        return false;  // COW로 표시되지 않은 페이지에 대한 진짜 쓰기 권한 위반
    }

    const kernel::uint64_t oldPhys = ptEntry & kAddrMask;
    const kernel::uint64_t newPhys = kernel::PageFrameAllocator::allocPage();
    if (!newPhys) {
        return false;  // OOM - 매핑해줄 방법이 없으니 그대로 패닉시킨다
    }

    // 공유돼 있던 원본 내용을 그대로 복사한 뒤(direct map을 거쳐 두
    // 물리 프레임 모두에 커널이 접근), 새 프레임을 이 주소공간에만
    // 쓰기 가능(WRITABLE)/COW 아님으로 다시 매핑한다 - 다른 주소공간이
    // 여전히 원래 oldPhys를 공유하고 있어도 전혀 영향받지 않는다.
    memcpy(reinterpret_cast<void*>(kernel::kPhysToVirt(newPhys)),
           reinterpret_cast<void*>(kernel::kPhysToVirt(oldPhys)), kPageSize4K);

    const kernel::uint64_t preservedFlags = (ptEntry & kLeafFlagsMask) & ~kernel::PAGE_WRITABLE;
    pt[ptIndex] = newPhys | preservedFlags | kernel::PAGE_PRESENT | kernel::PAGE_WRITABLE;
    kInvalidatePage(va);

    // [완료, 2026-09-19, PN-610CA401, SP-6CEFBE9B §6.2] rmap "이동" -
    // PN-44C91D6E(fork())가 완료되며 "착수 시 함께 배선"하겠다고
    // 예고해 둔 잔여 항목(RM-F2DAFF66 §1-K). 이 프로세스는 이제
    // oldPhys를 더 이상 매핑하지 않고 newPhys를 매핑하므로, rmap
    // 리스트도 그대로 따라가야 한다 - 안 그러면 oldPhys에 이 프로세스/
    // 가상주소를 가리키는 스테일 엔트리가 남고 newPhys는 rmap이 비어
    // 회수 후보 판단에서 조용히 빠진다(둘 다 지금은 관찰 가능한
    // 버그가 아니다 - rmap 소비자(swap 스캔, PN-4859FDE9)가 아직
    // 없어서다). `#PF`는 그 폴트를 낸 코드가 실행 중이던 바로 그
    // 주소공간에서만 발생하므로(이 함수 문서 주석 그대로) 이 시점의
    // `Scheduler::currentTask()`는 항상 이 COW 프레임을 실제로
    // 매핑 중이던 그 UserThread 자신이다(kHandleUserBreakpointHit
    // 등 다른 원시 ISR/예외 컨텍스트 함수와 동일한 전제 - AsyncTask
    // onExec()의 "currentTask() 오용" 함정(PN-5BBD4301)과는 다른
    // 상황: 그건 리액터가 나중에 다른 코어/컨텍스트에서 대신 실행하는
    // 비동기 경로라 currentTask()가 제출자를 안 가리키는 문제였지만,
    // #PF는 그 자체가 폴트를 낸 Task의 동기적 실행 흐름 안이라 항상
    // 정확하다).
    kernel::Task* faultingTask = kernel::Scheduler::currentTask();
    kernel::Process* owner = nullptr;
    if (faultingTask && faultingTask->isUserLevel) {
        auto* faultingThread = static_cast<kernel::UserThread*>(faultingTask);
        kernel::SharedPtr<kernel::Process> proc = faultingThread->process.lock();
        owner = proc.get();
    }
    kernel::PageFrameAllocator::removeRmap(oldPhys, owner, va);
    kernel::PageFrameAllocator::insertRmap(newPhys, owner, va);

    // oldPhys 몫의 공유 참조를 하나 반납한다 - retain()/freePage의
    // 기존 카운팅 관례 그대로(0이 되지 않는 한 실제 반납 안 됨, 다른
    // 주소공간이 여전히 이 프레임을 갖고 있으면 그쪽 몫은 그대로 남음).
    // removeRmap()을 freePage() 이전에 먼저 부른다 - PageFrameAllocator
    // 문서 주석(removeRmap)이 명시한 순서 그대로.
    kernel::PageFrameAllocator::freePage(oldPhys);
    return true;
}

// [신규, 2026-09-23, PN-4859FDE9 §7.2 5단계] `PAGE_RECLAIM_INPROGRESS`
// 쓰기 폴트 처리 - kHandleCowWriteFault와 마찬가지로 항상 현재(CR3)
// 주소공간 기준. present이면서 `PAGE_WRITABLE`이 꺼져 있는데 COW도
// 아니면(이 코드베이스에서 그 조합을 만드는 건 이 비트뿐) 이 경로다 -
// 새 park/resume 없이 그 자리에서 즉시 회수를 취소하고 재시도시킨다
// (paging.h의 PAGE_RECLAIM_INPROGRESS 문서 주석 참고 - 비동기 쓰기가
// 나중에 끝나면 완료 핸들러가 `kPageFrameFlagReclaimCanceled`를 보고
// 스왑 전환을 건너뛴다). 대상이 애초에 이 비트가 아니면(테이블 경로가
// 끊겨 있거나 이미 취소/완료된 뒤 등) false.
bool kHandleReclaimWriteFault(kernel::uint64_t faultAddr) {
    const kernel::uint64_t va = faultAddr & ~(kPageSize4K - 1);
    const kernel::uint64_t pml4Phys = kCurrentPml4Phys();

    kernel::Spinlock* lock = kLockForAddressSpaceOp(va, pml4Phys);
    kernel::uint64_t phys = 0;
    {
        kernel::SpinlockGuard guard(*lock);
        const kernel::uint64_t* pte = kFindLeafPteLocked(va, pml4Phys);
        if (!pte || !(*pte & kernel::PAGE_PRESENT) || !(*pte & kernel::PAGE_RECLAIM_INPROGRESS)) {
            return false;
        }
        phys = *pte & kAddrMask;
    }
    // 위 임계구역을 빠져나온 뒤 PageFrameAllocator::cancelReclaimForFrame()이
    // 그 자신의 gLruLock을 잡고 다시 주소공간 락을 잡는다(문서화된
    // 락 순서 "gLruLock -> 주소공간 락"을 그대로 지키기 위해 - 이
    // 함수가 먼저 주소공간 락을 쥔 채로 gLruLock을 요구하면 순서가
    // 뒤집힌다). phys가 가리키는 PageFrame이 이미 free된 극단적 경우
    // (레이스로 인한 방어적 처리)엔 frameFor()가 nullptr을 돌려주므로
    // cancelReclaimForFrame() 자체가 안전하게 아무 일도 안 한다.
    kernel::PageFrame* frame = kernel::PageFrameAllocator::frameFor(phys);
    if (frame) {
        kernel::PageFrameAllocator::cancelReclaimForFrame(frame);
    } else {
        kernel::Paging::cancelReclaimInProgress(va, pml4Phys);
    }
    return true;
}

// [신규, 2026-09-23, PN-4859FDE9 §4.2] 스왑인 읽기 - onExec() 자신의
// 완료 시점에 PTE 설치/rmap 삽입/파킹된 스레드 깨우기까지 전부 처리한다
// (swapfs_io.h의 SwapWriteHandler/SwapReadHandler는 "순수 슬롯 읽기/
// 쓰기"만 하고 호출부별 후속 조치를 의도적으로 안 하는데, 이 호출부
// (#PF 핸들러)는 그 후속 조치를 관측할 "제출자"가 없다(fire-and-forget,
// Syscall::submitDetached()와 동일한 관례) - 그래서 그 두 핸들러를
// 재사용하지 않고 슬롯->LBA 산출까지 포함해 이 핸들러 하나로 완결
// 짓는다).
struct SwapInArgs {
    fs::BlockDevice* device = nullptr;
    fs::SwapSlot slot = 0;
    kernel::uint64_t newPhys = 0;    // 이미 할당된 목적지 프레임 - kPhysToVirt로 직접 읽어들임
    kernel::uint64_t virtAddr = 0;   // 폴트난 가상주소(4K 정렬)
    kernel::uint64_t pml4Phys = 0;
    kernel::WeakPtr<kernel::Task> parkedThread;  // parkFromISR로 파킹된 스레드(스레드가 그 사이 죽었을 수 있어 Weak)
    kernel::uint32_t resumeCoreHint = 0;          // 원래 폴트난 코어 - 깨울 때 그 코어 큐로
};

class SwapInReadHandler : public kernel::AsyncTaskHandler {
public:
    kernel::AsyncExecCoro onExec(kernel::AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<SwapInArgs*>(argsRaw);

        const kernel::uint32_t blockSize = args->device->blockSize();
        const kernel::uint32_t blocksPerPage = static_cast<kernel::uint32_t>(fs::kSwapPageSize / blockSize);
        const kernel::uint64_t lba = (args->slot * fs::kSwapPageSize) / blockSize;
        void* destPage = reinterpret_cast<void*>(kernel::kPhysToVirt(args->newPhys));

        fs::BlockIoResult ioResult;
        kernel::AsyncTask* ioTask = args->device->submitReadBlocks(lba, destPage, blocksPerPage, &ioResult);
        if (ioTask) {
            co_await kernel::AsyncTaskCoroAwaiter(ioTask);
        } else {
            ioResult.ok = false;
        }

        // 읽기 성공 여부와 무관하게 파킹된 스레드는 반드시 깨워야 한다
        // (안 그러면 영원히 멈춰 있음) - 읽기가 실패했으면 PTE는 그대로
        // 스왑 마커로 남겨 두고 newPhys만 반납한다. 다음에 이 주소를
        // 다시 건드리면 또 폴트가 나 재시도된다(디스크 일시 오류류를
        // 이렇게 자연스럽게 재시도하게 됨 - v1은 영구 read 실패를
        // SIGSEGV로 승격시키는 로직까지는 안 둔다, RM-23F4B687 §4).
        bool installed = false;
        if (ioResult.ok) {
            installed = kernel::Paging::completeSwapIn(args->virtAddr, args->pml4Phys, args->newPhys);
            if (installed) {
                kernel::SharedPtr<kernel::Task> t = args->parkedThread.lock();
                kernel::Process* owner = nullptr;
                if (t && t->isUserLevel) {
                    auto* thread = static_cast<kernel::UserThread*>(t.get());
                    kernel::SharedPtr<kernel::Process> proc = thread->process.lock();
                    owner = proc.get();
                }
                if (owner) {
                    kernel::PageFrameAllocator::insertRmap(args->newPhys, owner, args->virtAddr);
                }
            }
        }
        if (!installed) {
            // 실패했거나(디스크 오류) 경쟁하던 다른 스왑인이 먼저
            // 끝났거나 - 어느 쪽이든 이 핸들러가 확보해 둔 프레임은
            // 더 이상 쓸모없다.
            kernel::PageFrameAllocator::freePage(args->newPhys);
        }

        kernel::SharedPtr<kernel::Task> waiter = args->parkedThread.lock();
        if (waiter) {
            kernel::Scheduler::enqueue(args->resumeCoreHint, waiter.get());
        }
        // waiter가 없으면(스레드가 파킹된 채로 죽음 - 프로세스 강제
        // 종료 등) 깨울 대상 자체가 없다 - 정직한 v1 한계로 남겨 둔다
        // (그 스레드의 다른 정리 경로가 이미 처리했을 것이라는 전제,
        // 프로세스 종료와 이 회수 경로의 상호작용은 PN-4859FDE9 본문
        // 참고).

        kernel::GenericSlabAllocator::free(args, sizeof(SwapInArgs));
        co_return;
    }
    void onFailure(kernel::AsyncTask*) override {}
    void onCancel(kernel::AsyncTask*, void*) override {}
};

SwapInReadHandler gSwapInReadHandler;
kernel::AsyncTaskSubjectCode gSwapInReadSubjectCode = 0;
bool gSwapInReadHandlerRegistered = false;

kernel::AsyncTaskSubjectCode kEnsureSwapInReadHandlerRegistered() {
    if (!gSwapInReadHandlerRegistered) {
        gSwapInReadSubjectCode = kernel::AsyncCallbackRegistry::registerHandler(&gSwapInReadHandler);
        gSwapInReadHandlerRegistered = true;
    }
    return gSwapInReadSubjectCode;
}

// [신규, 2026-09-23, PN-4859FDE9 §4.2] not-present + PAGE_SWAP_MARKER
// 폴트를 실제로 처리 - 새 프레임을 확보하고 위 핸들러에 fire-and-forget
// 제출한 뒤 ParkForSwapIn을 돌려준다(park 자체는 idt.cpp가 gInPageFaultHandler
// 가드를 내린 뒤 수행 - paging.h 문서 주석 참고). 프레임 고갈/등록
// 실패면 NotHandled(호출부가 그대로 SIGSEGV로 떨어뜨림 - 스왑 영역이
// 있는데도 OOM이면 v1은 복구를 시도하지 않는다).
kernel::Paging::PageFaultOutcome kTrySubmitSwapIn(kernel::uint64_t faultAddr, kernel::uint64_t pml4Phys,
                                                    kernel::uint64_t slot) {
    fs::SwapBackend* backend = kernel::kActiveSwapBackend();
    if (!backend) {
        return kernel::Paging::PageFaultOutcome::NotHandled;  // 스왑 백엔드가 없는데 스왑 마커 PTE - 설계 불변식 위반, 방어적
    }
    const kernel::uint64_t newPhys = kernel::PageFrameAllocator::allocPage();
    if (!newPhys) {
        return kernel::Paging::PageFaultOutcome::NotHandled;
    }
    auto* args = static_cast<SwapInArgs*>(kernel::GenericSlabAllocator::alloc(sizeof(SwapInArgs)));
    if (!args) {
        kernel::PageFrameAllocator::freePage(newPhys);
        return kernel::Paging::PageFaultOutcome::NotHandled;
    }
    new (args) SwapInArgs();
    args->device = backend->device();
    args->slot = slot;
    args->newPhys = newPhys;
    args->virtAddr = faultAddr & ~(kPageSize4K - 1);
    args->pml4Phys = pml4Phys;
    // #PF는 항상 그 폴트를 낸 Task 자신의 동기적 실행 흐름 안이라
    // Scheduler::currentTask()가 항상 정확하고(kHandleCowWriteFault의
    // 문서 주석과 동일한 전제), ring3에서 온 폴트이므로 항상
    // UserThread다(weakAsTask()는 Task 자신이 아니라 UserThread/
    // KernelThread 각자가 EnableSharedFromThis로 따로 제공한다).
    auto* faultingThread = static_cast<kernel::UserThread*>(kernel::Scheduler::currentTask());
    args->parkedThread = faultingThread->weakAsTask();
    args->resumeCoreHint = kernel::Scheduler::currentCoreIndex();

    const kernel::AsyncTaskSubjectCode subjectCode = kEnsureSwapInReadHandlerRegistered();
    kernel::AsyncTask* task = kernel::AsyncTask::submit(subjectCode, 0, args, /*autoFree=*/true);
    if (!task) {
        args->~SwapInArgs();
        kernel::GenericSlabAllocator::free(args, sizeof(SwapInArgs));
        kernel::PageFrameAllocator::freePage(newPhys);
        return kernel::Paging::PageFaultOutcome::NotHandled;
    }
    return kernel::Paging::PageFaultOutcome::ParkForSwapIn;
}

}  // namespace

namespace kernel {

void Paging::init(uint64_t maxPhysAddr, uint64_t physicalBaseDelta) {
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
    // SP-CC2B18C6 §2 physicalBaseDelta 보정 - GRUB/PVH는 항상 0이라
    // 기존 값과 수치상 동일(무회귀).
    const uint64_t pdptPhys = pdptVirt - kKernelVma + physicalBaseDelta;

    const uint32_t pml4Index = kPml4Index(kDirectMapBase);
    pml4[pml4Index] = pdptPhys | PAGE_PRESENT | PAGE_WRITABLE;
}

}  // namespace

namespace {

// [수정, 2026-09-17, PN-90BD044E/DC-FB38F86F] Paging::mapPage()의 원래
// 본문 그대로 - 이제 "호출자가 이미 적절한 락(주소공간별 또는
// higher-half 전역)을 쥐고 있다"는 것을 전제하는 무잠금 내부 구현으로
// 옮겼다. Paging::mapRange()도 내부 루프에서 (재진입 불가 Spinlock을
// 다시 잡아 데드락 나는) Paging::mapPage()를 부르는 대신 이 함수를
// 직접 호출한다 - 락은 공개 진입점(Paging::mapPage/mapRange)에서
// 정확히 한 번만 잡는다.
void kMapPageUnlocked(kernel::uint64_t virtualAddr, kernel::uint64_t physicalAddr, kernel::uint64_t flags,
                      kernel::uint64_t pml4Phys) {
    virtualAddr &= ~(kPageSize4K - 1);
    physicalAddr &= ~(kPageSize4K - 1);

    kernel::uint64_t* pml4 = kAsTable(pml4Phys);
    kernel::uint64_t* pdpt = kGetOrCreateNextLevel(pml4, kPml4Index(virtualAddr), flags & kernel::PAGE_USER);
    kernel::uint64_t* pd = kGetOrCreateNextLevel(pdpt, kPdptIndex(virtualAddr), flags & kernel::PAGE_USER);
    // PL-57CF86EF 분할 - 이 4K 슬롯이 이미 2M 페이지에 속해 있으면
    // 아래서 그 엔트리를 PT 포인터로 잘못 해석하기 전에 먼저 풀어준다.
    kSplitTwoMegabyte(pd, kPdIndex(virtualAddr));
    kernel::uint64_t* pt = kGetOrCreateNextLevel(pd, kPdIndex(virtualAddr), flags & kernel::PAGE_USER);

    pt[kPtIndex(virtualAddr)] = physicalAddr | kernel::PAGE_PRESENT | flags;
    // 지금 실행 중인 주소공간(현재 CR3)에 대한 변경일 때만 TLB를
    // 무효화한다 - pml4Phys가 아직 CR3에 설치되지 않은 다른 주소공간을
    // 가리키면 이 코어의 TLB엔 애초에 그 매핑이 캐시돼 있을 수 없다
    // (invlpg는 항상 "지금 이 코어가 보고 있는 주소공간" 기준으로만
    // 의미가 있다).
    if (pml4Phys == kCurrentPml4Phys()) {
        kInvalidatePage(virtualAddr);
    }
}

}  // namespace

namespace kernel {

void Paging::initPatForThisCore() {
    arch::kWriteMsr64(kMsrPat, kPatValueWithIndex4Wc);
}

void Paging::mapPage(uint64_t virtualAddr, uint64_t physicalAddr, uint64_t flags, uint64_t pml4Phys) {
    if (pml4Phys == 0) {
        pml4Phys = kCurrentPml4Phys();
    }
    Spinlock* lock = kLockForAddressSpaceOp(virtualAddr, pml4Phys);
    SpinlockGuard guard(*lock);
    kMapPageUnlocked(virtualAddr, physicalAddr, flags, pml4Phys);
}

void Paging::mapRange(uint64_t virtualAddr, uint64_t physicalAddr, uint64_t sizeBytes, uint64_t flags, uint64_t pml4Phys) {
    if (pml4Phys == 0) {
        pml4Phys = kCurrentPml4Phys();
    }
    virtualAddr &= ~(kPageSize4K - 1);
    physicalAddr &= ~(kPageSize4K - 1);
    sizeBytes = (sizeBytes + kPageSize4K - 1) & ~(kPageSize4K - 1);

    // [PN-90BD044E/DC-FB38F86F] 락 획득 범위 결정 - 이 범위 전체를
    // 하나의 락으로 감싼다(개별 4K/2M 조각마다 다시 잡지 않음). 한
    // 호출의 [virtualAddr, virtualAddr+sizeBytes) 범위는 항상 단일
    // VMA/세그먼트 하나에 대응해 higher-half/lower-half 경계를 넘지
    // 않는다는 이 커널의 기존 메모리 레이아웃 전제(SP-8B6B8D25) 위에서,
    // 시작 주소 하나로 고른 락이 범위 전체에 유효하다고 본다.
    Spinlock* lock = kLockForAddressSpaceOp(virtualAddr, pml4Phys);
    SpinlockGuard guard(*lock);

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
                // 뭔가 있으면(2M이든 부분적으로 채워진 4K PT든) 기존
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
        kMapPageUnlocked(va, pa, flags, pml4Phys);
        mapped += kPageSize4K;
    }
}

bool Paging::mergeRange(uint64_t virtualAddr, uint64_t sizeBytes, uint64_t pml4Phys) {
    if (pml4Phys == 0) {
        pml4Phys = kCurrentPml4Phys();
    }
    // [PN-90BD044E/DC-FB38F86F] 이 함수는 kGetOrCreateNextLevel()을
    // 안 쓰지만, mapPage()/mapRange()가 만드는 것과 같은 PD/PT 엔트리를
    // 직접 읽고 재구성(PT -> 2M PS로 교체)하므로 그 둘과 똑같이
    // 주소공간별/higher-half 락으로 보호해야 한다 - 안 그러면 다른
    // 코어의 mapPage()가 병합 도중인 PT를 동시에 채우다 그 갱신이
    // 유실될 수 있다.
    Spinlock* lock = kLockForAddressSpaceOp(virtualAddr, pml4Phys);
    SpinlockGuard guard(*lock);

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
            continue;  // 병합 불변 조건 불만족 - 재배치(경로 b)는 이번 범위 밖, 그대로 둔다
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

Paging::PageFaultOutcome Paging::handlePageFault(uint64_t faultAddr, uint64_t errorCode) {
    constexpr uint64_t kErrorCodePresentBit = 1UL << 0;
    constexpr uint64_t kErrorCodeWriteBit = 1UL << 1;
    if (errorCode & kErrorCodePresentBit) {
        // 이미 매핑된 페이지에 대한 위반 - [PN-543C0CE9 착수 6번째
        // 증분] COW, [신규, PN-4859FDE9 §7.2 5단계] 회수 진행중 쓰기
        // 폴트 두 가지만 예외적으로 처리한다(§2/§11, PAGE_RECLAIM_INPROGRESS
        // 문서 주석). 그 외(쓰기가 아닌 위반, 둘 다 아닌 페이지에 대한
        // 위반)는 조용히 넘기지 않고 NotHandled로 패스스루한다.
        if (!(errorCode & kErrorCodeWriteBit)) {
            return PageFaultOutcome::NotHandled;
        }
        if (kHandleCowWriteFault(faultAddr)) {
            return PageFaultOutcome::Handled;
        }
        if (kHandleReclaimWriteFault(faultAddr)) {
            return PageFaultOutcome::Handled;
        }
        return PageFaultOutcome::NotHandled;
    }

    // [신규, 2026-09-23, PN-4859FDE9 §4.2] not-present 폴트 - 스왑
    // 마커인지 먼저 확인한다(지연 매핑 구역 검사보다 먼저 - 스왑아웃된
    // 유저 페이지는 kLazyZoneBase 범위 밖이므로 순서 자체는 서로
    // 배타적이지만, 스왑 마커 쪽이 개념상 더 구체적인 조건이라 먼저
    // 본다).
    {
        const uint64_t va = faultAddr & ~(kPageSize4K - 1);
        const uint64_t pml4Phys = kCurrentPml4Phys();
        Spinlock* lock = kLockForAddressSpaceOp(va, pml4Phys);
        bool isSwapped = false;
        uint64_t slot = 0;
        {
            SpinlockGuard guard(*lock);
            const uint64_t* pte = kFindLeafPteLocked(va, pml4Phys);
            if (pte && !(*pte & PAGE_PRESENT) && (*pte & PAGE_SWAP_MARKER)) {
                isSwapped = true;
                slot = kSwapSlotFromPte(*pte);
            }
        }
        if (isSwapped) {
            return kTrySubmitSwapIn(va, pml4Phys, slot);
        }
    }

    if (faultAddr < kLazyZoneBase || faultAddr >= kLazyZoneBase + kLazyZoneSize) {
        return PageFaultOutcome::NotHandled;  // 지연 매핑 구역 밖 - 진짜 잘못된 접근
    }

    const uint64_t phys = PageFrameAllocator::allocPage();
    if (!phys) {
        return PageFaultOutcome::NotHandled;  // OOM - 매핑해줄 방법이 없으니 그대로 패닉시킨다
    }

    mapPage(faultAddr & ~(kPageSize4K - 1), phys, PAGE_WRITABLE);
    return PageFaultOutcome::Handled;
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

bool Paging::testAndClearAccessed(uint64_t virtualAddr, uint64_t pml4Phys) {
    constexpr uint64_t kAccessedBit = 1UL << 5;

    virtualAddr &= ~(kPageSize4K - 1);
    if (pml4Phys == 0) {
        pml4Phys = kCurrentPml4Phys();
    }
    Spinlock* lock = kLockForAddressSpaceOp(virtualAddr, pml4Phys);
    SpinlockGuard guard(*lock);

    uint64_t* pml4 = kAsTable(pml4Phys);
    if (!(pml4[kPml4Index(virtualAddr)] & PAGE_PRESENT)) {
        return false;
    }
    uint64_t* pdpt = kAsTable(pml4[kPml4Index(virtualAddr)] & kAddrMask);
    if (!(pdpt[kPdptIndex(virtualAddr)] & PAGE_PRESENT)) {
        return false;
    }
    uint64_t* pd = kAsTable(pdpt[kPdptIndex(virtualAddr)] & kAddrMask);
    const uint64_t pdEntry = pd[kPdIndex(virtualAddr)];
    if (!(pdEntry & PAGE_PRESENT) || (pdEntry & kPageSizeBit)) {
        return false;  // 미매핑 또는 2MiB 대형 페이지 - 이 스캔의 범위 밖(위 문서 주석 참고)
    }
    uint64_t* pt = kAsTable(pdEntry & kAddrMask);
    uint64_t& pte = pt[kPtIndex(virtualAddr)];
    if (!(pte & PAGE_PRESENT)) {
        return false;
    }
    const bool wasAccessed = (pte & kAccessedBit) != 0;
    if (wasAccessed) {
        pte &= ~kAccessedBit;
        if (pml4Phys == kCurrentPml4Phys()) {
            kInvalidatePage(virtualAddr);
        }
    }
    return wasAccessed;
}

bool Paging::markReclaimInProgress(uint64_t virtualAddr, uint64_t pml4Phys) {
    virtualAddr &= ~(kPageSize4K - 1);
    Spinlock* lock = kLockForAddressSpaceOp(virtualAddr, pml4Phys);
    SpinlockGuard guard(*lock);

    uint64_t* pte = kFindLeafPteLocked(virtualAddr, pml4Phys);
    if (!pte || !(*pte & PAGE_PRESENT) || (*pte & PAGE_RECLAIM_INPROGRESS)) {
        return false;
    }
    *pte = (*pte & ~PAGE_WRITABLE) | PAGE_RECLAIM_INPROGRESS;
    TlbShootdown::broadcast(virtualAddr, virtualAddr + kPageSize4K, pml4Phys);
    return true;
}

void Paging::cancelReclaimInProgress(uint64_t virtualAddr, uint64_t pml4Phys) {
    virtualAddr &= ~(kPageSize4K - 1);
    Spinlock* lock = kLockForAddressSpaceOp(virtualAddr, pml4Phys);
    SpinlockGuard guard(*lock);

    uint64_t* pte = kFindLeafPteLocked(virtualAddr, pml4Phys);
    if (!pte || !(*pte & PAGE_RECLAIM_INPROGRESS)) {
        return;
    }
    *pte = (*pte & ~PAGE_RECLAIM_INPROGRESS) | PAGE_WRITABLE;
    TlbShootdown::broadcast(virtualAddr, virtualAddr + kPageSize4K, pml4Phys);
}

void Paging::finalizeReclaimToSwap(uint64_t virtualAddr, uint64_t pml4Phys, uint64_t slot) {
    virtualAddr &= ~(kPageSize4K - 1);
    Spinlock* lock = kLockForAddressSpaceOp(virtualAddr, pml4Phys);
    SpinlockGuard guard(*lock);

    uint64_t* pte = kFindLeafPteLocked(virtualAddr, pml4Phys);
    if (!pte || !(*pte & PAGE_RECLAIM_INPROGRESS)) {
        return;  // 이미 취소됨(쓰기 폴트가 먼저 원복) - 호출부가 flags로 알아서 건너뜀
    }
    *pte = kMakeSwapPte(slot);
    TlbShootdown::broadcast(virtualAddr, virtualAddr + kPageSize4K, pml4Phys);
}

bool Paging::completeSwapIn(uint64_t virtualAddr, uint64_t pml4Phys, uint64_t newPhys) {
    virtualAddr &= ~(kPageSize4K - 1);
    Spinlock* lock = kLockForAddressSpaceOp(virtualAddr, pml4Phys);
    SpinlockGuard guard(*lock);

    uint64_t* pte = kFindLeafPteLocked(virtualAddr, pml4Phys);
    if (!pte) {
        return false;
    }
    if (*pte & PAGE_PRESENT) {
        return false;  // 경쟁하던 다른 스왑인이 먼저 끝남 - 호출부가 newPhys를 반납해야 함
    }
    *pte = newPhys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    // not-present -> present로 새로 매핑하는 것뿐이라(이전엔 캐싱될
    // 수조차 없었던 자리) 크로스 코어 무효화가 필요 없다(mapPage의
    // 지연 매핑 경로와 동일한 근거) - 로컬 TLB도 애초에 이 주소를
    // 캐싱한 적이 없으므로 invlpg조차 불필요.
    return true;
}

bool Paging::isUserRangeValid(uint64_t virtualAddr, uint64_t length, uint64_t pml4Phys) {
    if (length == 0) {
        return true;
    }
    const uint64_t end = virtualAddr + length;
    if (end < virtualAddr) {
        return false;  // 오버플로우 - 악의적/잘못된 (addr,length) 조합
    }
    if (pml4Phys == 0) {
        pml4Phys = kCurrentPml4Phys();
    }

    const uint64_t alignedStart = virtualAddr & ~(kPageSize4K - 1);
    const uint64_t alignedEnd = (end + kPageSize4K - 1) & ~(kPageSize4K - 1);
    for (uint64_t addr = alignedStart; addr < alignedEnd; addr += kPageSize4K) {
        uint64_t* pml4 = kAsTable(pml4Phys);
        const uint64_t pml4Entry = pml4[kPml4Index(addr)];
        if (!(pml4Entry & PAGE_PRESENT) || !(pml4Entry & PAGE_USER)) {
            return false;
        }
        uint64_t* pdpt = kAsTable(pml4Entry & kAddrMask);
        const uint64_t pdptEntry = pdpt[kPdptIndex(addr)];
        if (!(pdptEntry & PAGE_PRESENT) || !(pdptEntry & PAGE_USER)) {
            return false;
        }
        uint64_t* pd = kAsTable(pdptEntry & kAddrMask);
        const uint64_t pdEntry = pd[kPdIndex(addr)];
        if (!(pdEntry & PAGE_PRESENT) || !(pdEntry & PAGE_USER)) {
            return false;
        }
        if (pdEntry & kPageSizeBit) {
            continue;  // 2MiB 대형 페이지 - PD 엔트리 자체가 이미 leaf
        }
        uint64_t* pt = kAsTable(pdEntry & kAddrMask);
        const uint64_t ptEntry = pt[kPtIndex(addr)];
        if (!(ptEntry & PAGE_PRESENT) || !(ptEntry & PAGE_USER)) {
            return false;
        }
    }
    return true;
}

// [신규, 2026-09-19, PN-C6CDC26A] paging.h 문서 주석 참고 - 4KiB
// 경계까지만 유효를 보장한다(translatePage()가 2MiB 거대 페이지를
// 만나도 그 안의 정확한 오프셋을 돌려주지만, 이 함수는 debug_session.cpp
// kCopyDebuggeeMemory()와 동일한 관례로 항상 4KiB 단위로만 잘라
// 돌려준다 - 단순함 우선, RM-23F4B687 §4).
void* kResolveUserPointer(uint64_t pml4Phys, uint64_t userVa, uint64_t size, uint64_t* outValidLen) {
    if (size == 0) {
        return nullptr;
    }
    const uint64_t pageBase = userVa & ~(kPageSize4K - 1);
    const uint64_t pageOffset = userVa - pageBase;
    uint64_t validLen = kPageSize4K - pageOffset;
    if (validLen > size) {
        validLen = size;
    }
    if (!Paging::isUserRangeValid(userVa, validLen, pml4Phys)) {
        return nullptr;
    }
    const uint64_t phys = Paging::translatePage(pageBase, pml4Phys);
    if (!phys) {
        return nullptr;
    }
    // translatePage()가 이미 pageBase를 4KiB로 재정렬해 버리므로
    // (2MiB 거대 페이지 오프셋 계산은 이 함수가 항상 4KiB로만 잘라
    // 쓰는 이상 여기서 필요 없다), 여기서 pageOffset을 직접 더한다.
    if (outValidLen) {
        *outValidLen = validLen;
    }
    return reinterpret_cast<void*>(kPhysToVirt(phys) + pageOffset);
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

    // [PN-90BD044E/DC-FB38F86F] 이 pml4Phys를 향한 mapPage()/mapRange()/
    // mergeRange() 호출이 앞으로 kLockForAddressSpaceOp()로 락을 찾을
    // 수 있도록 여기서 미리 등록해 둔다 - 등록 자체가 실패(레지스트리
    // 슬랩 고갈)하면 이 주소공간을 아예 안전하게 쓸 수 없다고 보고
    // PageFrameAllocator 고갈과 동일하게 실패 처리한다(이미 확보한
    // PML4 프레임은 반납).
    if (!kFindOrCreateAddressSpaceLock(newPml4Phys)) {
        PageFrameAllocator::freePage(newPml4Phys);
        return 0;
    }
    return newPml4Phys;
}

void Paging::destroyAddressSpace(uint64_t pml4Phys) {
    uint64_t* pml4 = kAsTable(pml4Phys);
    for (uint32_t i = 0; i < kHigherHalfPml4Start; ++i) {
        if (pml4[i] & PAGE_PRESENT) {
            kFreeUserPageTablesRecursive(pml4[i] & kAddrMask, 3);
        }
    }
    PageFrameAllocator::freePage(pml4Phys);
    // [PN-90BD044E/DC-FB38F86F] createAddressSpace()가 등록해 둔 락
    // 항목을 반납한다 - 이 pml4Phys는 이제 반납됐으니 등록을 안 지우면
    // 나중에 PageFrameAllocator가 같은 물리 프레임을 다른 새 주소공간에
    // 재할당했을 때 엉뚱하게 옛 항목을 "찾아서" 재사용하게 된다(그
    // 자체가 틀린 동작은 아니지만 - 락 인스턴스는 동일하게 안전 -
    // 레지스트리가 이미 죽은 주소공간의 항목을 무한정 쌓아 두는 누수를
    // 막기 위해 명시적으로 정리한다).
    kRemoveAddressSpaceLock(pml4Phys);
}

// [신규, 2026-09-18, SP-8D206F11 §2.3] CacheType -> PAT/PCD/PWT 비트
// 변환표 - Paging::initPatForThisCore()가 세팅한 IA32_PAT 레이아웃과
// 정확히 대응한다(인덱스0/1/3/4).
void kMapPageWithCacheType(uint64_t virtAddr, uint64_t physAddr, uint64_t flags, CacheType cacheType,
                            uint64_t pml4Phys) {
    switch (cacheType) {
        case CacheType::WriteBack:
            break;  // 인덱스0 - PAT.PCD.PWT 전부 0, 추가 비트 없음
        case CacheType::WriteThrough:
            flags |= PAGE_WRITE_THROUGH;  // 인덱스1
            break;
        case CacheType::Uncached:
            flags |= PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE;  // 인덱스3
            break;
        case CacheType::WriteCombining:
            flags |= PAGE_PAT;  // 인덱스4
            break;
    }
    Paging::mapPage(virtAddr, physAddr, flags, pml4Phys);
}

}  // namespace kernel
