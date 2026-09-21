#include "dma_buffer.h"

#include "address_space.h"
#include "libkenv/spinlock.h"
#include "libkmm/slab.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "process.h"
#include "task.h"

namespace kernel {

namespace {

constexpr uint64_t kDmaPageSize = 4096;

// [신규, 2026-09-22, PN-A8BE8BED 항목3, QU-0C2CB097/QU-F8004FD5] 커널
// 모드(KernelThread) 호출자를 위한 DMA 버퍼 가상주소 스크래치 할당자 -
// 유저모드 경로(ProcessAddressSpaceManager::mapRegion)와 구조적으로
// 대응하지만 KernelAddressSpaceManager(address_space.h)는 재사용하지
// 않는다: 그 관리자의 MapleTree는 동시에 최대 8개 VMA만 추적 가능한데
// (address_space.h 클래스 문서 참고), AHCI 한 포트만으로도 NCQ
// 슬롯마다(최대 32개, PN-A401DDF9) 커맨드테이블+데이터버퍼가 필요해
// 동시 할당 수가 그 한계를 훨씬 넘는다 - 그래서 이 용도 전용의 별도
// 고정 가상주소 범위(kKernelDriverMmioScratchVirtBase/pnp.cpp,
// kMsixTableVirtBase/pci.cpp 등 기존 고정 MMIO 슬롯들과 같은 관례)와
// 별도 자료구조(VMA/COW/lazy fault 개념이 전혀 필요 없는 단순 정렬
// free-list)를 쓴다.
//
// [설계 재확인, QU-F8004FD5 답변("유저모드 드라이버는 보류")이
// 이 설계 자체의 확정 답변인지 모호해 QU-0C2CB097로 범위를 재확인 -
// "커널 자신의 DMA 버퍼 설계는 그대로 진행해"(2026-09-21)로 확정]
// handle→항목 매핑은 QU-F8004FD5가 열어 둔 두 후보(KernelThread에
// 필드 추가 vs 이 할당자 자신이 겸함) 중 후자로 결정했다(착수 세션
// 확정 가능한 구현 디테일, RM-23F4B687 §4 - libkvdb 로그인명 인덱스
// 선택과 같은 패턴) - 지금 유일한 소비자(fs)를 포함해 커널 모드 DMA
// 버퍼를 "소유"할 만한 KernelThread 개념 자체가 아직 없고(Process
// 없는 순수 Task, SP-43331889 §1), 여러 KernelThread가 생겨도 이
// 모듈 하나가 계속 겸하는 편이 새 필드를 얹는 것보다 단순하다.
constexpr uint64_t kKernelDmaScratchVirtBase = 0xFFFF901000200000UL;  // pci.cpp의 kMsixTableVirtBase(0x110000, 1페이지) 다음 여유를 두고 시작 - 이 파일 밑 static_assert가 실제 겹침을 막는다
constexpr uint64_t kKernelDmaScratchSize = 64UL * 1024 * 1024;  // 64MiB - 가상주소 공간은 물리 메모리와 달리 저렴해 v1부터 넉넉히 예약
constexpr uint64_t kKernelDmaScratchPageCount = kKernelDmaScratchSize / kDmaPageSize;

// 스크래치 범위 안의 빈 페이지 구간 하나(pageOffset/pageCount는 페이지
// 단위) - pageOffset 오름차순으로 정렬된 단일 연결 리스트로 유지해
// free()가 인접 구간과 즉시 병합할 수 있게 한다. 병합이 필요한 이유는
// 가정이 아니다 - 이 기능의 유일한 소비자(AHCI, ahci.cpp)가 매 I/O
// 명령마다 커맨드테이블+데이터버퍼를 alloc+free하는 게 그 자체로
// 주된 사용 패턴이라, 병합 없이는 반복 실행 중 빠르게 단편화된다.
struct DmaScratchFreeRun {
    uint64_t pageOffset = 0;
    uint64_t pageCount = 0;
    DmaScratchFreeRun* next = nullptr;
};

Spinlock gKernelDmaLock;
DmaScratchFreeRun* gDmaScratchFreeList = nullptr;
bool gDmaScratchInited = false;

// [caller: gKernelDmaLock 보유 상태] 최초 호출 시 전체 범위를 가리키는
// free run 하나로 초기화한다 - 커널 힙(GenericSlabAllocator)이 이미
// 갖춰진 뒤(fs의 첫 AllocDmaBuffer 호출 시점)에나 불리므로 부팅 순서를
// 별도로 타지 않는다(지연 초기화, PageFrameAllocator::init() 같은
// 전용 부팅 단계를 새로 추가하지 않기 위함 - RM-23F4B687 §4).
void kEnsureDmaScratchInitLocked() {
    if (gDmaScratchInited) {
        return;
    }
    auto* run = static_cast<DmaScratchFreeRun*>(GenericSlabAllocator::alloc(sizeof(DmaScratchFreeRun)));
    if (run) {
        *run = DmaScratchFreeRun{};
        run->pageCount = kKernelDmaScratchPageCount;
        gDmaScratchFreeList = run;
    }
    gDmaScratchInited = true;
}

// [caller: gKernelDmaLock 보유 상태] first-fit 탐색 - 필요하면 찾은
// run을 쪼갠다(정확히 맞으면 리스트에서 제거).
bool kDmaScratchAllocPagesLocked(uint64_t pageCount, uint64_t* outPageOffset) {
    DmaScratchFreeRun* prevNode = nullptr;
    for (DmaScratchFreeRun* run = gDmaScratchFreeList; run != nullptr; run = run->next) {
        if (run->pageCount >= pageCount) {
            *outPageOffset = run->pageOffset;
            if (run->pageCount == pageCount) {
                if (prevNode) {
                    prevNode->next = run->next;
                } else {
                    gDmaScratchFreeList = run->next;
                }
                GenericSlabAllocator::free(run, sizeof(DmaScratchFreeRun));
            } else {
                run->pageOffset += pageCount;
                run->pageCount -= pageCount;
            }
            return true;
        }
        prevNode = run;
    }
    return false;
}

// [caller: gKernelDmaLock 보유 상태] 정렬 삽입 + 앞/뒤 인접 구간 병합.
void kDmaScratchFreePagesLocked(uint64_t pageOffset, uint64_t pageCount) {
    DmaScratchFreeRun* prevNode = nullptr;
    DmaScratchFreeRun* curNode = gDmaScratchFreeList;
    while (curNode != nullptr && curNode->pageOffset < pageOffset) {
        prevNode = curNode;
        curNode = curNode->next;
    }

    DmaScratchFreeRun* mergedNode = nullptr;
    if (curNode != nullptr && pageOffset + pageCount == curNode->pageOffset) {
        // 뒤쪽 구간과 붙는다 - curNode를 그대로 확장해 흡수.
        curNode->pageOffset = pageOffset;
        curNode->pageCount += pageCount;
        mergedNode = curNode;
    } else {
        auto* newRun = static_cast<DmaScratchFreeRun*>(GenericSlabAllocator::alloc(sizeof(DmaScratchFreeRun)));
        if (!newRun) {
            // 슬랩 고갈 - 이 가상주소 구간은 회수하지 못하고 샌다(극히
            // 드묾, 방어적 처리 - 새 에러 경로를 늘리지 않는다는
            // RM-23F4B687 §4 판단을 물리 프레임 쪽이 아니라 가상주소
            // 장부 쪽에도 그대로 적용).
            return;
        }
        newRun->pageOffset = pageOffset;
        newRun->pageCount = pageCount;
        newRun->next = curNode;
        if (prevNode) {
            prevNode->next = newRun;
        } else {
            gDmaScratchFreeList = newRun;
        }
        mergedNode = newRun;
    }

    if (prevNode != nullptr && prevNode->pageOffset + prevNode->pageCount == mergedNode->pageOffset) {
        // 앞쪽 구간과도 붙는다 - prevNode로 흡수하고 mergedNode는 반납.
        prevNode->pageCount += mergedNode->pageCount;
        prevNode->next = mergedNode->next;
        GenericSlabAllocator::free(mergedNode, sizeof(DmaScratchFreeRun));
    }
}

// 커널 모드 DMA 버퍼 handle→항목 장부 - Process::dmaBuffers(유저모드)와
// 대응하지만, 이 버퍼들을 "소유"하는 Process가 없어(devmgr/fs는 Process
// 없는 순수 Task) 이 모듈 자신이 겸한다(위 파일 서두 주석 참고). v1은
// pnp.cpp/libkvdb와 동일한 관례로 고정 배열 + 선형 탐색 - 단일 AHCI
// 컨트롤러의 실사용치(포트당 CLB/FB 2개 + NCQ 슬롯 최대 32개 x
// 커맨드테이블/데이터버퍼 2개 = 66개)를 넉넉히 상회한다.
constexpr uint32_t kKernelDmaBufferSlotCount = 256;

struct KernelDmaBufferSlot {
    bool used = false;
    uint64_t physAddr = 0;
    uint64_t virtAddr = 0;
    uint32_t pageCount = 0;
    uint32_t handle = 0;
};

KernelDmaBufferSlot gKernelDmaBufferSlots[kKernelDmaBufferSlotCount];

// [caller: gKernelDmaLock 보유 상태] Process::DmaBuffer 진영의
// kAllocateDmaHandle()과 동일한 관례(0은 무효 핸들로 남김) - 다만 이
// 모듈 전역 배열을 대상으로 한다.
uint32_t kAllocateKernelDmaHandleLocked() {
    for (uint32_t candidate = 1; candidate != 0; ++candidate) {
        bool inUse = false;
        for (uint32_t i = 0; i < kKernelDmaBufferSlotCount; ++i) {
            if (gKernelDmaBufferSlots[i].used && gKernelDmaBufferSlots[i].handle == candidate) {
                inUse = true;
                break;
            }
        }
        if (!inUse) {
            return candidate;
        }
    }
    return 0;
}

KernelDmaBufferSlot* kFindFreeKernelDmaSlotLocked() {
    for (uint32_t i = 0; i < kKernelDmaBufferSlotCount; ++i) {
        if (!gKernelDmaBufferSlots[i].used) {
            return &gKernelDmaBufferSlots[i];
        }
    }
    return nullptr;
}

KernelDmaBufferSlot* kFindKernelDmaSlotByHandleLocked(uint32_t handle) {
    for (uint32_t i = 0; i < kKernelDmaBufferSlotCount; ++i) {
        if (gKernelDmaBufferSlots[i].used && gKernelDmaBufferSlots[i].handle == handle) {
            return &gKernelDmaBufferSlots[i];
        }
    }
    return nullptr;
}

// [vfs_syscall.cpp의 kAllocateFd와 동일한 패턴 재사용] 이 프로세스가
// 아직 안 쓰는 가장 작은 양의 handle을 찾는다 - 0은 "무효 핸들"로
// 남겨 둔다(AllocDmaBufferArgs::handle 문서 주석과 동일한 관례,
// FileDescriptor의 -1 sentinel과 대응).
uint32_t kAllocateDmaHandle(Process* process) {
    for (uint32_t candidate = 1; candidate != 0; ++candidate) {
        if (!process->dmaBuffers.find(
                [candidate](const Process::DmaBuffer& e) { return e.handle == candidate; })) {
            return candidate;
        }
    }
    return 0;
}

uint32_t kOrderForPageCount(uint32_t pageCount) {
    uint32_t order = 0;
    while ((1u << order) < pageCount) {
        ++order;
    }
    return order;
}

// [SP-39F18E30 §2/§3.1] devmgr(또는 그 드라이버 자식)이 컨트롤러 DMA
// 구조체 하나를 만들 때마다 호출 - 물리적으로 연속인 페이지를 확보해
// 그 프로세스 주소공간에 매핑하고, 물리주소/핸들을 함께 내준다.
class AllocDmaBufferHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<AllocDmaBufferArgs*>(argsRaw);
        SharedPtr<Task> caller = task->submitterTask.lock();
        if (!caller) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        kAllocDmaBufferSync(caller, args->sizeBytes, args->physAddrLimit, &args->virtualAddr, &args->physicalAddr,
                            &args->handle, &args->error);
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

AllocDmaBufferHandler gAllocDmaBufferHandler;

class FreeDmaBufferHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<FreeDmaBufferArgs*>(argsRaw);
        SharedPtr<Task> caller = task->submitterTask.lock();
        if (!caller) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        kFreeDmaBufferSync(caller, args->handle, &args->error);
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

FreeDmaBufferHandler gFreeDmaBufferHandler;

}  // namespace

// [신규, 2026-09-20, SP-43331889 §7(fs 전환)] dma_buffer.h 선언 참고 -
// User-Level 분기는 `AllocDmaBufferHandler::onExec()`의 기존 본문
// 그대로(Process::addressSpace/Process::dmaBuffers), Kernel-Level
// 분기는 설계자 지시로 명시적 미구현(ChannelError::NotSupported).
void kAllocDmaBufferSync(const SharedPtr<Task>& caller, uint64_t sizeBytes, uint32_t physAddrLimit,
                          uint64_t* outVirtualAddr, uint64_t* outPhysicalAddr, uint32_t* outHandle,
                          ChannelError* outError) {
    if (sizeBytes == 0) {
        *outError = ChannelError::InvalidArgument;
        return;
    }

    if (!caller->isUserLevel) {
        // [구현, 2026-09-22, PN-A8BE8BED 항목3, QU-0C2CB097 답변("커널
        // 자신의 DMA 버퍼 설계는 그대로 진행해")] 위 파일 서두의
        // kKernelDmaScratchVirtBase 관련 주석 참고 - physmap 직접 재사용
        // (캐시 일관성 문제)과 pnp.cpp류 고정 슬롯 하나(AHCI NCQ 동시성
        // 요구에 못 미침) 둘 다 QU-F8004FD5에서 부적합함을 실측/코드
        // 확인 후, 유저모드 경로와 구조적으로 대응하는 전용 스크래치
        // 할당자로 구현했다.
        const uint64_t pageCount = (sizeBytes + kDmaPageSize - 1) / kDmaPageSize;
        const uint32_t order = kOrderForPageCount(static_cast<uint32_t>(pageCount));
        const uint64_t blockPageCount = 1ULL << order;

        const uint64_t physAddr = (physAddrLimit == 32)
                                       ? PageFrameAllocator::allocOrderBelow(0x1'0000'0000ULL, order)
                                       : PageFrameAllocator::allocOrder(order);
        if (!physAddr) {
            *outError = ChannelError::ResourceExhausted;
            return;
        }

        uint64_t pageOffset = 0;
        bool gotVirt = false;
        {
            SpinlockGuard guard(gKernelDmaLock);
            kEnsureDmaScratchInitLocked();
            gotVirt = kDmaScratchAllocPagesLocked(blockPageCount, &pageOffset);
        }
        if (!gotVirt) {
            PageFrameAllocator::freeOrder(physAddr, order);
            *outError = ChannelError::ResourceExhausted;
            return;
        }

        // [pnp.cpp의 kMapMmioForCaller Kernel-Level 분기와 동일한 전제]
        // devmgr/fs는 Process 없는 순수 커널 Task라 자신만의 PML4가
        // 따로 없다 - 항상 부팅 때 확정된 단일 커널 PML4 위에서 실행되므로
        // (SP-43331889 §1, QU-ECEE5990 답변 B), Paging::mapPage()를
        // pml4Phys 없이(현재 CR3) 불러도 KernelAddressSpaceManager::init()
        // 이 경계했던 "이 매핑을 다른 PML4가 못 볼 수 있다"는 위험이
        // 애초에 적용되지 않는다(대상 자체가 유저 프로세스별 PML4가 아님).
        for (uint64_t i = 0; i < blockPageCount; ++i) {
            Paging::mapPage(kKernelDmaScratchVirtBase + (pageOffset + i) * kDmaPageSize, physAddr + i * kDmaPageSize,
                             PAGE_WRITABLE | PAGE_CACHE_DISABLE);
        }
        const uint64_t virtAddr = kKernelDmaScratchVirtBase + pageOffset * kDmaPageSize;

        uint32_t handle = 0;
        KernelDmaBufferSlot* slot = nullptr;
        {
            SpinlockGuard guard(gKernelDmaLock);
            slot = kFindFreeKernelDmaSlotLocked();
            if (slot) {
                handle = kAllocateKernelDmaHandleLocked();
            }
            if (slot && handle != 0) {
                slot->used = true;
                slot->physAddr = physAddr;
                slot->virtAddr = virtAddr;
                slot->pageCount = static_cast<uint32_t>(blockPageCount);
                slot->handle = handle;
            }
        }
        if (!slot || handle == 0) {
            for (uint64_t i = 0; i < blockPageCount; ++i) {
                Paging::unmapPage(virtAddr + i * kDmaPageSize);
            }
            {
                SpinlockGuard guard(gKernelDmaLock);
                kDmaScratchFreePagesLocked(pageOffset, blockPageCount);
            }
            PageFrameAllocator::freeOrder(physAddr, order);
            *outError = ChannelError::ResourceExhausted;
            return;
        }

        *outVirtualAddr = virtAddr;
        *outPhysicalAddr = physAddr;
        *outHandle = handle;
        *outError = ChannelError::None;
        return;
    }

    SharedPtr<Process> process = kOwnerProcessOf(caller.get());
    if (!process) {
        *outError = ChannelError::InvalidHandle;
        return;
    }

    // §3.1 1번 - 4KiB 페이지 수로 올림 -> 그 값을 담을 최소 order.
    const uint64_t pageCount = (sizeBytes + kDmaPageSize - 1) / kDmaPageSize;
    const uint32_t order = kOrderForPageCount(static_cast<uint32_t>(pageCount));

    // §3.1 2번 - physAddrLimit==32면 allocOrderBelow(§5-B), 아니면
    // 기존 allocOrder.
    const uint64_t physAddr = (physAddrLimit == 32) ? PageFrameAllocator::allocOrderBelow(0x1'0000'0000ULL, order)
                                                     : PageFrameAllocator::allocOrder(order);
    if (!physAddr) {
        *outError = ChannelError::ResourceExhausted;
        return;
    }

    // §3.1 3번 - ProcessAddressSpaceManager::mapRegion() 한 번으로
    // findGap+Paging::mapPage+장부 등록까지 마친다(RequestIoPermissionHandler,
    // pnp.cpp와 동일한 패턴 - 캐시 속성은 §3.3이 제안한 PAGE_CACHE_DISABLE).
    uint64_t virtAddr = 0;
    const uint64_t mappedLength = kDmaPageSize << order;
    if (!process->addressSpace.mapRegion(mappedLength, PAGE_WRITABLE | PAGE_USER | PAGE_CACHE_DISABLE,
                                          VmaBacking::FixedPhysical, physAddr, &virtAddr)) {
        PageFrameAllocator::freeOrder(physAddr, order);
        *outError = ChannelError::ResourceExhausted;
        return;
    }

    // §3.1 4번 - Process::dmaBuffers에 {physAddr, virtAddr, pageCount,
    // handle}을 기록하고 handle을 발급.
    process->dmaBuffers.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    const uint32_t handle = kAllocateDmaHandle(process.get());
    if (handle == 0) {
        process->addressSpace.unmapRegion(virtAddr, mappedLength);
        PageFrameAllocator::freeOrder(physAddr, order);
        *outError = ChannelError::ResourceExhausted;
        return;
    }

    Process::DmaBuffer entry;
    entry.physAddr = physAddr;
    entry.virtAddr = virtAddr;
    entry.pageCount = static_cast<uint32_t>(1ULL << order);
    entry.handle = handle;
    entry.used = true;
    process->dmaBuffers.insert(entry);

    *outVirtualAddr = virtAddr;
    *outPhysicalAddr = physAddr;
    *outHandle = handle;
    *outError = ChannelError::None;
}

void kFreeDmaBufferSync(const SharedPtr<Task>& caller, uint32_t handle, ChannelError* outError) {
    if (!caller->isUserLevel) {
        // [구현, 2026-09-22, PN-A8BE8BED 항목3] 위 kAllocDmaBufferSync
        // Kernel-Level 분기와 대응 - handle 조회는 gKernelDmaBufferSlots
        // 장부, 가상주소 반납은 kDmaScratchFreePagesLocked.
        uint64_t virtAddr = 0;
        uint64_t physAddr = 0;
        uint32_t pageCount = 0;
        bool found = false;
        {
            SpinlockGuard guard(gKernelDmaLock);
            if (KernelDmaBufferSlot* slot = kFindKernelDmaSlotByHandleLocked(handle)) {
                virtAddr = slot->virtAddr;
                physAddr = slot->physAddr;
                pageCount = slot->pageCount;
                found = true;
            }
        }
        if (!found) {
            *outError = ChannelError::InvalidHandle;
            return;
        }

        for (uint32_t i = 0; i < pageCount; ++i) {
            Paging::unmapPage(virtAddr + static_cast<uint64_t>(i) * kDmaPageSize);
        }
        PageFrameAllocator::freeOrder(physAddr, kOrderForPageCount(pageCount));

        {
            SpinlockGuard guard(gKernelDmaLock);
            kDmaScratchFreePagesLocked((virtAddr - kKernelDmaScratchVirtBase) / kDmaPageSize, pageCount);
            if (KernelDmaBufferSlot* slot = kFindKernelDmaSlotByHandleLocked(handle)) {
                slot->used = false;
            }
        }

        *outError = ChannelError::None;
        return;
    }

    SharedPtr<Process> process = kOwnerProcessOf(caller.get());
    if (!process) {
        *outError = ChannelError::InvalidHandle;
        return;
    }

    auto* slot = process->dmaBuffers.find([handle](const Process::DmaBuffer& e) { return e.handle == handle; });
    if (!slot) {
        *outError = ChannelError::InvalidHandle;
        return;
    }

    const uint32_t order = kOrderForPageCount(slot->value.pageCount);
    process->addressSpace.unmapRegion(slot->value.virtAddr,
                                       static_cast<uint64_t>(slot->value.pageCount) * kDmaPageSize);
    PageFrameAllocator::freeOrder(slot->value.physAddr, order);
    process->dmaBuffers.erase(slot);

    *outError = ChannelError::None;
}

void DmaBufferService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointAllocDmaBuffer, &gAllocDmaBufferHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointFreeDmaBuffer, &gFreeDmaBufferHandler);
}

}  // namespace kernel
