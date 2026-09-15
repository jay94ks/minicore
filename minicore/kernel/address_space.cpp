#include "address_space.h"

#include "libkmm/slab.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "tlb_shootdown.h"

namespace {

constexpr kernel::uint64_t kPageSize4K = 0x1000UL;

kernel::uint64_t kAlignUp4K(kernel::uint64_t size) {
    return (size + kPageSize4K - 1) & ~(kPageSize4K - 1);
}

// Anonymous 백킹으로 [start, start+lengthMapped) 몇 페이지를 이미
// 매핑해 둔 상태에서 실패해 되돌릴 때 공용으로 쓴다 - mapRegion()의
// 두 실패 지점(페이지 고갈 도중/tree.store 포화)이 정확히 같은 롤백을
// 반복해야 해서 헬퍼로 뺐다. FixedPhysical은 프레임을 반납하지 않는다
// (소유권이 호출부에 있음, address_space.h의 문서 주석 참고).
void kRollbackMapped(kernel::uint64_t pml4Phys, kernel::uint64_t start, kernel::uint64_t mappedBytes,
                     kernel::VmaBacking backing) {
    for (kernel::uint64_t off = 0; off < mappedBytes; off += kPageSize4K) {
        const kernel::uint64_t physAddr = kernel::Paging::translatePage(start + off, pml4Phys);
        kernel::Paging::unmapPage(start + off, pml4Phys);
        if (backing == kernel::VmaBacking::Anonymous && physAddr) {
            kernel::PageFrameAllocator::freePage(physAddr);
        }
    }
}

}  // namespace

namespace kernel {

void ProcessAddressSpaceManager::init(uint64_t pml4Phys, uint64_t regionFloor, uint64_t regionCeil) {
    _pml4Phys = pml4Phys;
    _regionFloor = regionFloor;
    _regionCeil = regionCeil;
    _tree.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    _tree.init();
}

bool ProcessAddressSpaceManager::mapRegion(uint64_t length, uint64_t prot, VmaBacking backing,
                                            uint64_t fixedPhysAddr, uint64_t* outAddr) {
    if (length == 0 || !outAddr) {
        return false;
    }
    const uint64_t lengthAligned = kAlignUp4K(length);

    SpinlockGuard guard(_lock);

    uint64_t start = 0;
    if (!_tree.findGap(_regionFloor, _regionCeil, lengthAligned, &start)) {
        return false;
    }

    auto* vma = static_cast<Vma*>(GenericSlabAllocator::alloc(sizeof(Vma)));
    if (!vma) {
        return false;
    }
    *vma = Vma{};
    vma->start = start;
    vma->end = start + lengthAligned - 1;
    vma->prot = prot;
    vma->backing = backing;
    vma->fixedPhysAddr = fixedPhysAddr;

    uint64_t mappedBytes = 0;
    for (; mappedBytes < lengthAligned; mappedBytes += kPageSize4K) {
        uint64_t physAddr;
        if (backing == VmaBacking::Anonymous) {
            physAddr = PageFrameAllocator::allocPage();
            if (!physAddr) {
                break;
            }
        } else {
            physAddr = fixedPhysAddr + mappedBytes;
        }
        Paging::mapPage(start + mappedBytes, physAddr, prot | PAGE_USER, _pml4Phys);
    }

    if (mappedBytes < lengthAligned) {
        kRollbackMapped(_pml4Phys, start, mappedBytes, backing);
        GenericSlabAllocator::free(vma, sizeof(Vma));
        return false;
    }

    if (!_tree.store(vma->start, vma->end, vma)) {
        // v1 MapleTree는 최대 kMapleArangeSlotCount(10)개 엔트리로
        // 제한된다(§6-5) - 이미 매핑한 페이지 전부 롤백.
        kRollbackMapped(_pml4Phys, start, lengthAligned, backing);
        GenericSlabAllocator::free(vma, sizeof(Vma));
        return false;
    }

    *outAddr = start;
    return true;
}

bool ProcessAddressSpaceManager::unmapRegion(uint64_t addr, uint64_t length) {
    if (length == 0) {
        return false;
    }
    const uint64_t lengthAligned = kAlignUp4K(length);
    const uint64_t alignedAddr = addr & ~(kPageSize4K - 1);

    SpinlockGuard guard(_lock);

    uint64_t rangeStart = 0;
    uint64_t rangeEnd = 0;
    void* value = _tree.find(alignedAddr, &rangeStart, &rangeEnd);
    if (!value || rangeStart != alignedAddr || rangeEnd != alignedAddr + lengthAligned - 1) {
        return false;  // v1은 mapRegion()이 반환한 범위 그대로만 지원(부분 unmap 미지원)
    }
    auto* vma = static_cast<Vma*>(value);

    kRollbackMapped(_pml4Phys, alignedAddr, lengthAligned, vma->backing);
    _tree.erase(rangeStart, rangeEnd);
    GenericSlabAllocator::free(vma, sizeof(Vma));
    return true;
}

bool ProcessAddressSpaceManager::registerFixedRegion(uint64_t start, uint64_t length, uint64_t prot,
                                                      VmaBacking backing) {
    if (length == 0) {
        return false;
    }

    SpinlockGuard guard(_lock);

    auto* vma = static_cast<Vma*>(GenericSlabAllocator::alloc(sizeof(Vma)));
    if (!vma) {
        return false;
    }
    *vma = Vma{};
    vma->start = start;
    vma->end = start + length - 1;
    vma->prot = prot;
    vma->backing = backing;

    if (!_tree.store(vma->start, vma->end, vma)) {
        // 실제 페이지 매핑은 호출부가 이미 끝냈다 - 이 함수는 그 사실을
        // 장부에 못 남긴 것뿐이라, 여기서는 되돌릴 매핑이 없다(위 클래스
        // 선언부 문서 주석 참고 - 되돌림은 호출부 책임).
        GenericSlabAllocator::free(vma, sizeof(Vma));
        return false;
    }
    return true;
}

void ProcessAddressSpaceManager::unmapAll() {
    SpinlockGuard guard(_lock);

    // forEach 도중 tree.erase를 직접 호출하면 순회 중인 스냅샷 자체를
    // 바꾸게 되므로, 먼저 전부 수집한 뒤 트리 밖에서 처리한다(v1은
    // 최대 kMapleArangeSlotCount(10)개라 스택 배열로 충분).
    struct Entry {
        uint64_t start;
        uint64_t end;
        Vma* vma;
    };
    Entry entries[kMapleArangeSlotCount];
    uint32_t count = 0;
    _tree.forEach([&](uint64_t start, uint64_t end, void* value) {
        if (count < kMapleArangeSlotCount) {
            entries[count++] = Entry{start, end, static_cast<Vma*>(value)};
        }
    });

    for (uint32_t i = 0; i < count; ++i) {
        kRollbackMapped(_pml4Phys, entries[i].start, entries[i].end - entries[i].start + 1, entries[i].vma->backing);
        GenericSlabAllocator::free(entries[i].vma, sizeof(Vma));
    }
    _tree.init();  // 루트 자체를 반납해 빈 상태로 되돌린다(재사용 대비)
}

namespace {

Spinlock gKernelAddressSpaceLock;
MapleTree gKernelAddressSpaceTree;

}  // namespace

void KernelAddressSpaceManager::init() {
    gKernelAddressSpaceTree.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    gKernelAddressSpaceTree.init();
}

bool KernelAddressSpaceManager::mapRegion(uint64_t length, uint64_t flags, uint64_t* outAddr) {
    if (length == 0 || !outAddr) {
        return false;
    }
    const uint64_t lengthAligned = kAlignUp4K(length);

    SpinlockGuard guard(gKernelAddressSpaceLock);

    uint64_t start = 0;
    if (!gKernelAddressSpaceTree.findGap(kLazyZoneBase, kLazyZoneBase + kLazyZoneSize - 1, lengthAligned, &start)) {
        return false;
    }

    auto* vma = static_cast<Vma*>(GenericSlabAllocator::alloc(sizeof(Vma)));
    if (!vma) {
        return false;
    }
    *vma = Vma{};
    vma->start = start;
    vma->end = start + lengthAligned - 1;
    vma->prot = flags;
    vma->backing = VmaBacking::Anonymous;

    // 커널 영역은 모든 PML4가 higher-half를 공유하므로(Paging::
    // createAddressSpace) pml4Phys=0(현재 CR3)으로 매핑해도 다른 모든
    // 프로세스 주소공간에서 즉시 같은 매핑이 보인다 - 어느 프로세스
    // 컨텍스트에서 이 함수를 부르든 상관없다.
    uint64_t mappedBytes = 0;
    for (; mappedBytes < lengthAligned; mappedBytes += kPageSize4K) {
        const uint64_t physAddr = PageFrameAllocator::allocPage();
        if (!physAddr) {
            break;
        }
        Paging::mapPage(start + mappedBytes, physAddr, flags, 0);
    }

    if (mappedBytes < lengthAligned) {
        kRollbackMapped(0, start, mappedBytes, VmaBacking::Anonymous);
        GenericSlabAllocator::free(vma, sizeof(Vma));
        return false;
    }

    if (!gKernelAddressSpaceTree.store(vma->start, vma->end, vma)) {
        kRollbackMapped(0, start, lengthAligned, VmaBacking::Anonymous);
        GenericSlabAllocator::free(vma, sizeof(Vma));
        return false;
    }

    *outAddr = start;
    return true;
}

bool KernelAddressSpaceManager::unmapRegion(uint64_t addr, uint64_t length) {
    if (length == 0) {
        return false;
    }
    const uint64_t lengthAligned = kAlignUp4K(length);
    const uint64_t alignedAddr = addr & ~(kPageSize4K - 1);

    SpinlockGuard guard(gKernelAddressSpaceLock);

    uint64_t rangeStart = 0;
    uint64_t rangeEnd = 0;
    void* value = gKernelAddressSpaceTree.find(alignedAddr, &rangeStart, &rangeEnd);
    if (!value || rangeStart != alignedAddr || rangeEnd != alignedAddr + lengthAligned - 1) {
        return false;
    }
    auto* vma = static_cast<Vma*>(value);

    kRollbackMapped(0, alignedAddr, lengthAligned, vma->backing);
    gKernelAddressSpaceTree.erase(rangeStart, rangeEnd);
    GenericSlabAllocator::free(vma, sizeof(Vma));

    // 이 락을 쥔 채로 broadcast - tlb_shootdown.h의 호출 요구사항
    // 그대로(§3, SP-DE19BB1C).
    TlbShootdown::broadcast(alignedAddr, alignedAddr + lengthAligned);
    return true;
}

}  // namespace kernel
