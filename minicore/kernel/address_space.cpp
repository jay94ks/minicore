#include "address_space.h"

#include "libkmm/slab.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "process.h"
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
// (소유권이 호출부에 있음, address_space.h의 문서 주석 참고). owner:
// [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §6.2] Anonymous 프레임을
// 실제로 반납(`freePage`)하기 **전에** 그 (owner, vaddr) rmap 엔트리를
// 먼저 지운다 - `KernelAddressSpaceManager`(소유 프로세스 개념이 없음)
// 호출부는 항상 nullptr을 넘긴다(그 경우 rmap 자체가 애초에 삽입된
// 적이 없으므로 제거도 조용히 생략).
void kRollbackMapped(kernel::uint64_t pml4Phys, kernel::uint64_t start, kernel::uint64_t mappedBytes,
                     kernel::VmaBacking backing, kernel::Process* owner) {
    for (kernel::uint64_t off = 0; off < mappedBytes; off += kPageSize4K) {
        const kernel::uint64_t physAddr = kernel::Paging::translatePage(start + off, pml4Phys);
        kernel::Paging::unmapPage(start + off, pml4Phys);
        if (backing == kernel::VmaBacking::Anonymous && physAddr) {
            if (owner) {
                kernel::PageFrameAllocator::removeRmap(physAddr, owner, start + off);
            }
            kernel::PageFrameAllocator::freePage(physAddr);
        }
    }
}

}  // namespace

namespace kernel {

void ProcessAddressSpaceManager::init(uint64_t pml4Phys, uint64_t regionFloor, uint64_t regionCeil,
                                       Process* owner) {
    _pml4Phys = pml4Phys;
    _regionFloor = regionFloor;
    _regionCeil = regionCeil;
    _owner = owner;
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
        // [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §6.2] 방금 실제로
        // 매핑한 Anonymous 페이지의 rmap 삽입 - "요구 페이징 폴트나
        // COW 폴트가 PTE에 매핑하는 순간"과 동급인 v1의 실제 매핑
        // 시점(§6.2 "삽입" 원문 그대로, 여기는 즉시 매핑이라 그 시점이
        // 바로 지금이다).
        if (backing == VmaBacking::Anonymous) {
            PageFrameAllocator::insertRmap(physAddr, _owner, start + mappedBytes);
        }
    }

    if (mappedBytes < lengthAligned) {
        kRollbackMapped(_pml4Phys, start, mappedBytes, backing, _owner);
        GenericSlabAllocator::free(vma, sizeof(Vma));
        return false;
    }

    if (!_tree.store(vma->start, vma->end, vma)) {
        // v1 MapleTree는 최대 kMapleArangeSlotCount(10)개 엔트리로
        // 제한된다(§6-5) - 이미 매핑한 페이지 전부 롤백.
        kRollbackMapped(_pml4Phys, start, lengthAligned, backing, _owner);
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

    kRollbackMapped(_pml4Phys, alignedAddr, lengthAligned, vma->backing, _owner);
    _tree.erase(rangeStart, rangeEnd);
    GenericSlabAllocator::free(vma, sizeof(Vma));

    // [PN-D132A1E9/QU-DE2828A1] 이 락을 쥔 채로 broadcast - 다른
    // 코어가 이 프로세스를 지금 실행 중이면 방금 해제한 페이지의
    // 스테일 TLB 엔트리를 그대로 들고 있을 수 있다. targetPml4Phys를
    // 넘기면 Active CPU Mask(이 pml4를 실제로 실행 중인 코어만)로
    // 좁혀서 브로드캐스트한다 - 커널 영역 전체 브로드캐스트가 아니다.
    TlbShootdown::broadcast(alignedAddr, alignedAddr + lengthAligned, _pml4Phys);
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

    // [신규, 2026-09-18, PN-2FC5ED36, SP-6CEFBE9B §6.2] 이 함수는
    // `mapRegion()`과 달리 페이지를 직접 매핑하지 않는다(호출부 - ELF
    // 로더/유저 스택 설정 - 가 이미 끝내 둠, 위 클래스 선언부 문서
    // 주석 참고) - 그래서 rmap 삽입도 매핑 루프 안이 아니라 여기서
    // 등록된 범위를 다시 훑으며 한다. `Paging::translatePage()`로
    // 호출부가 이미 매핑해 둔 물리주소를 그대로 읽어올 뿐, 새로
    // 매핑/할당하지 않는다.
    if (backing == VmaBacking::Anonymous) {
        for (uint64_t off = 0; off < length; off += kPageSize4K) {
            const uint64_t physAddr = Paging::translatePage(start + off, _pml4Phys);
            if (physAddr) {
                PageFrameAllocator::insertRmap(physAddr, _owner, start + off);
            }
        }
    }
    return true;
}

bool ProcessAddressSpaceManager::resizeAnonymousRegion(uint64_t start, uint64_t oldLength, uint64_t newLength) {
    const uint64_t oldLenAligned = kAlignUp4K(oldLength);
    const uint64_t newLenAligned = kAlignUp4K(newLength);
    if (oldLenAligned == newLenAligned) {
        return true;  // 실질적 변화 없음
    }

    SpinlockGuard guard(_lock);

    uint64_t rangeStart = 0;
    uint64_t rangeEnd = 0;
    void* value = _tree.find(start, &rangeStart, &rangeEnd);
    if (!value || rangeStart != start || rangeEnd != start + oldLenAligned - 1) {
        return false;  // 등록된 적 없거나 범위 불일치
    }
    auto* vma = static_cast<Vma*>(value);
    if (vma->backing != VmaBacking::Anonymous) {
        return false;  // v1은 Anonymous 힙만 지원
    }

    if (newLenAligned > oldLenAligned) {
        uint64_t mappedDelta = 0;
        for (; mappedDelta < newLenAligned - oldLenAligned; mappedDelta += kPageSize4K) {
            const uint64_t physAddr = PageFrameAllocator::allocPage();
            if (!physAddr) {
                break;
            }
            const uint64_t vaddr = start + oldLenAligned + mappedDelta;
            Paging::mapPage(vaddr, physAddr, vma->prot | PAGE_USER, _pml4Phys);
            PageFrameAllocator::insertRmap(physAddr, _owner, vaddr);
        }
        if (mappedDelta < newLenAligned - oldLenAligned) {
            kRollbackMapped(_pml4Phys, start + oldLenAligned, mappedDelta, VmaBacking::Anonymous, _owner);
            return false;
        }
    } else {
        kRollbackMapped(_pml4Phys, start + newLenAligned, oldLenAligned - newLenAligned, VmaBacking::Anonymous,
                         _owner);
        // [PN-D132A1E9/QU-DE2828A1] 힙 축소(Brk 감소)로 실제로
        // 언맵된 범위만 - 성장 경로는 이전에 없던 주소에 새로 매핑할
        // 뿐이라 다른 코어가 그 주소의 stale 매핑을 들고 있을 수
        // 없으므로 shootdown이 필요 없다.
        TlbShootdown::broadcast(start + newLenAligned, start + oldLenAligned, _pml4Phys);
    }

    _tree.erase(rangeStart, rangeEnd);
    if (!_tree.store(start, start + newLenAligned - 1, vma)) {
        // 트리 갱신 실패(극히 드묾 - 엔트리 수는 그대로인 재삽입이라
        // 포화 가능성은 낮지만 방어적으로 처리) - 실제로 바뀐 페이지
        // 매핑까지 원래 상태로 되돌린 뒤(성장이었다면 방금 새로 매핑한
        // 델타를 다시 해제, 축소였다면 이미 해제해 버린 페이지는
        // 되살릴 수 없어 그대로 남긴다 - 이 경우 tree가 old 범위로
        // 복구돼도 VMA 크기와 실제 매핑 상태가 어긋나는 잔여 위험이
        // 있다, 실측된 적 없어 새 DC 없이 주석으로만 남김) 원래
        // 범위로 재등록을 시도한다.
        if (newLenAligned > oldLenAligned) {
            kRollbackMapped(_pml4Phys, start + oldLenAligned, newLenAligned - oldLenAligned, VmaBacking::Anonymous,
                             _owner);
        }
        _tree.store(rangeStart, rangeEnd, vma);
        return false;
    }
    vma->end = start + newLenAligned - 1;
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
        kRollbackMapped(_pml4Phys, entries[i].start, entries[i].end - entries[i].start + 1, entries[i].vma->backing,
                         _owner);
        GenericSlabAllocator::free(entries[i].vma, sizeof(Vma));
    }
    // [수정, 2026-09-16, PN-7FF5DA89 QA 중 발견] 예전엔 `_tree.init()`을
    // 불러 루트를 반납과 동시에 새로 하나 재할당했다 - `unmapAll()`의
    // 유일한 호출부(`Process::destroy()`)가 그 뒤로 Resurrect(`Process::
    // init()`이 `addressSpace.init()`->`_tree.init()`을 다시 불러 안전
    // 하게 정리/재할당)되지 않고 그대로 `Process::release()`로 영구
    // 반납되면, 이 마지막 빈 루트 노드 하나가 매번 새는 잠재 버그였다
    // (`MapleTree::destroy()` 문서 주석 참고 - 실제로 QEMU 반복 실행
    // 중 `PageFrameAllocator::freePageCount()`가 조금씩 줄어드는 것으로
    // 발견). `destroy()`는 반납만 하고 재할당하지 않는다 - Resurrect
    // 경로는 다음 `init()`이 `_root==nullptr`을 보고 알아서 새로
    // 할당하므로 동작 변화 없음.
    _tree.destroy();
}

namespace {

Spinlock gKernelAddressSpaceLock;
MapleTree gKernelAddressSpaceTree;

// SP-2AAD7C8D §2.1(QU-4E9F1C36 설계자 답변, 2026-09-15) - 이 관리자가
// 조작하는 건 항상 "모든 PML4가 공유하는 상위 레벨 테이블"이어야
// 한다는 걸 코드 레벨에서 보장하기 위해, "현재 CR3"가 아니라 이
// 마스터 PML4를 명시적으로 타깃팅한다. `init()`은 `TlbShootdown::
// init()` 직후, 어떤 프로세스도 생성되기 전에 BSP에서 한 번만
// 호출됨이 부팅 시퀀스로 보장돼 있으므로(kmain.cpp), 이 시점의
// `Paging::currentPml4Phys()`는 항상 부팅 PML4다(scheduler.cpp가
// `gBootPml4Phys`를 캡처하는 것과 정확히 같은 논리 - 그 전역을 이
// 파일에서 직접 참조할 순 없어 여기서 독립적으로 한 번 더 캡처한다).
uint64_t gKernelMasterPml4Phys = 0;

}  // namespace

void KernelAddressSpaceManager::init() {
    gKernelAddressSpaceTree.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    gKernelAddressSpaceTree.init();

    gKernelMasterPml4Phys = Paging::currentPml4Phys();

    // **비판적 재검토로 발견된 잠복 버그의 수정(QU-4E9F1C36, SP-2AAD7C8D
    // §2.1)**: `Paging::createAddressSpace()`는 프로세스 생성 "시점"의
    // PML4 상위 절반(256~511)을 엔트리째로 한 번만 스냅샷 복사한다 -
    // `kLazyZoneBase`가 속한 PML4 슬롯이 그 어떤 프로세스보다도 먼저
    // present 상태가 아니면, `mapRegion()`의 첫 실제 호출이 하필 어떤
    // 유저 프로세스의 CR3 위에서 일어나는 순간 그 프로세스 하나의
    // PML4에만 매핑이 생기고 이미 존재하는 다른 프로세스/부팅 PML4는
    // 그 매핑을 영원히 모르게 된다. 그래서 여기서 더미 매핑+즉시 해제
    // 왕복으로 PML4E(및 그 아래 PDPT/PD/PT 체인)를 미리 만들어 둔다 -
    // `unmapPage()`는 leaf PTE만 지우고 중간 테이블 자체는 그대로
    // 남기므로(kRollbackMapped와 동일한 전제), 이후 어떤
    // `createAddressSpace()`가 이 슬롯을 스냅샷해 가도 이미 유효한
    // PDPT를 공유하게 된다.
    const uint64_t dummyPhys = PageFrameAllocator::allocPage();
    if (dummyPhys) {
        Paging::mapPage(kLazyZoneBase, dummyPhys, PAGE_WRITABLE, gKernelMasterPml4Phys);
        Paging::unmapPage(kLazyZoneBase, gKernelMasterPml4Phys);
        PageFrameAllocator::freePage(dummyPhys);
    }
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

    // **`gKernelMasterPml4Phys`를 명시적으로 타깃팅한다**(SP-2AAD7C8D
    // §2.1 - QU-4E9F1C36 방어적 보강) - `init()`이 이미 이 PML4 슬롯의
    // PDPT를 선점해 둬서(위 주석 참고) 어차피 모든 PML4가 이 슬롯을
    // 공유하므로 "현재 CR3"에 매핑해도 기능상으로는 동일하지만,
    // "이 호출은 항상 공유 상위 테이블만 조작한다"를 우연한 동작이
    // 아니라 코드 레벨에서 보장하기 위해 매번 이 값을 명시한다.
    uint64_t mappedBytes = 0;
    for (; mappedBytes < lengthAligned; mappedBytes += kPageSize4K) {
        const uint64_t physAddr = PageFrameAllocator::allocPage();
        if (!physAddr) {
            break;
        }
        Paging::mapPage(start + mappedBytes, physAddr, flags, gKernelMasterPml4Phys);
    }

    if (mappedBytes < lengthAligned) {
        kRollbackMapped(gKernelMasterPml4Phys, start, mappedBytes, VmaBacking::Anonymous, nullptr);
        GenericSlabAllocator::free(vma, sizeof(Vma));
        return false;
    }

    if (!gKernelAddressSpaceTree.store(vma->start, vma->end, vma)) {
        kRollbackMapped(gKernelMasterPml4Phys, start, lengthAligned, VmaBacking::Anonymous, nullptr);
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

    kRollbackMapped(gKernelMasterPml4Phys, alignedAddr, lengthAligned, vma->backing, nullptr);
    gKernelAddressSpaceTree.erase(rangeStart, rangeEnd);
    GenericSlabAllocator::free(vma, sizeof(Vma));

    // 이 락을 쥔 채로 broadcast - tlb_shootdown.h의 호출 요구사항
    // 그대로(§3, SP-DE19BB1C).
    TlbShootdown::broadcast(alignedAddr, alignedAddr + lengthAligned);
    return true;
}

namespace {

// 세 핸들러 전부 channel.cpp의 OpenChannelHandler 관례를 그대로
// 따른다 - 실제 대기가 필요 없는 순수 동기 작업(AsyncTask::yield()
// 없이 onExec 안에서 즉시 끝남)이라 가장 단순한 형태다. **호출자
// 식별은 반드시 args->process를 쓴다 - Scheduler::currentTask()가
// 아니다**(실측으로 발견한 버그, 2026-09-16): onExec()이 실제로
// 실행되는 시점은 리액터가 idle 컨텍스트(PN-FEAAF154 이후
// gCurrentTask[coreIndex]==nullptr)에서 실행 큐를 드레인하는
// 순간이라, 그 안에서 새로 Scheduler::currentTask()를 부르면
// null이거나 완전히 엉뚱한 Task를 가리켜 GPF로 이어진다(address_space.h
// 의 MmapArgs 문서 주석 참고 - channel.cpp의 기존 핸들러들은 애초에
// 호출자 정보가 필요 없어 이 문제를 겪지 않았을 뿐이다).
class MmapHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<MmapArgs*>(argsRaw);
        if (args->length == 0 || !args->process) {
            args->error = AddressSpaceError::InvalidArgument;
            co_return;
        }
        uint64_t outAddr = 0;
        if (!args->process->addressSpace.mapRegion(args->length, args->prot, VmaBacking::Anonymous, 0,
                                                     &outAddr)) {
            args->error = AddressSpaceError::OutOfMemory;
            co_return;
        }
        args->addr = outAddr;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class MunmapHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<MunmapArgs*>(argsRaw);
        if (args->length == 0 || !args->process) {
            args->error = AddressSpaceError::InvalidArgument;
            co_return;
        }
        if (!args->process->addressSpace.unmapRegion(args->addr, args->length)) {
            args->error = AddressSpaceError::NotMapped;
        }
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

// SP-2AAD7C8D §5 "brk는 프로세스당 힙 VMA 하나를 미리 예약해 두고
// 그 끝점만 움직이는 전통적 구현" - Process::init()이 이미
// kMinHeapLength로 힙 VMA를 만들어 heapStart/heapBrk를 유효한 절대
// 주소로 확정해 뒀으므로(process.h 문서 주석 참고), 이 핸들러는
// POSIX brk(addr)와 동일하게 newBrk를 항상 절대 주소로 다룬다 -
// "최초 성장" 같은 특수 분기가 필요 없다. kMinHeapLength 밑으로는
// 축소할 수 없다(그 밑으로 내려가려면 최초 VMA 자체를 없애야 하는데
// 그걸 다시 만들 방법이 없다 - v1 제약, clamp로 처리).
class BrkHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<BrkArgs*>(argsRaw);
        Process* process = args->process;
        if (!process) {
            args->error = AddressSpaceError::InvalidArgument;
            co_return;
        }

        if (args->newBrk == 0) {
            args->currentBrk = process->heapBrk;
            co_return;
        }
        if (args->newBrk < process->heapStart) {
            args->error = AddressSpaceError::InvalidArgument;
            co_return;
        }

        const uint64_t oldLength = process->heapBrk - process->heapStart;
        uint64_t newLength = args->newBrk - process->heapStart;
        if (newLength < kMinHeapLength) {
            newLength = kMinHeapLength;
        }
        if (!process->addressSpace.resizeAnonymousRegion(process->heapStart, oldLength, newLength)) {
            args->error = AddressSpaceError::OutOfMemory;
            co_return;
        }
        process->heapBrk = process->heapStart + newLength;
        args->currentBrk = process->heapBrk;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

MmapHandler gMmapHandler;
MunmapHandler gMunmapHandler;
BrkHandler gBrkHandler;

}  // namespace

void registerAddressSpaceSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointMmap, &gMmapHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointMunmap, &gMunmapHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointBrk, &gBrkHandler);
}

}  // namespace kernel
