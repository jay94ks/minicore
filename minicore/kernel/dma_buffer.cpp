#include "dma_buffer.h"

#include "address_space.h"
#include "libkmm/slab.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "process.h"
#include "task.h"

namespace kernel {

namespace {

constexpr uint64_t kDmaPageSize = 4096;

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
        // [의도적 미구현, 2026-09-20, QU-5FC58B06 답변] dma_buffer.h
        // 문서 주석 참고 - 커널 모드 DMA 버퍼 매핑 설계 자체가 아직
        // 없다. 추측으로 채우지 않고 명확한 실패로 되돌린다.
        *outError = ChannelError::NotSupported;
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
        // AllocDmaBuffer의 Kernel-Level 분기가 항상 실패하므로(위 참고)
        // 커널 모드 호출자가 유효한 handle을 쥐고 있을 수 자체가 없다.
        *outError = ChannelError::NotSupported;
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
