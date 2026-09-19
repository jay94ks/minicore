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

// [pnp.cpp의 kProcessFromSubmitterForPnp(PN-9CC66142/DC-21647E46)와
// 동일한 패턴 재사용] "이 AsyncTask를 제출한 UserThread가 속한
// Process"를 얻는다 - channel.h가 이 헬퍼를 외부에 노출하지 않아 여기
// 다시 만든다(관계도에 중복 패턴으로 기록해 둠).
SharedPtr<Process> kProcessFromSubmitterForDma(AsyncTask* task) {
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter) {
        return SharedPtr<Process>();
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return thread->process.lock();
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

        if (args->sizeBytes == 0) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }

        SharedPtr<Process> process = kProcessFromSubmitterForDma(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }

        // §3.1 1번 - 4KiB 페이지 수로 올림 -> 그 값을 담을 최소 order.
        const uint64_t pageCount = (args->sizeBytes + kDmaPageSize - 1) / kDmaPageSize;
        const uint32_t order = kOrderForPageCount(static_cast<uint32_t>(pageCount));

        // §3.1 2번 - physAddrLimit==32면 allocOrderBelow(§5-B), 아니면
        // 기존 allocOrder.
        const uint64_t physAddr = (args->physAddrLimit == 32)
                                       ? PageFrameAllocator::allocOrderBelow(0x1'0000'0000ULL, order)
                                       : PageFrameAllocator::allocOrder(order);
        if (!physAddr) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }

        // §3.1 3번 - ProcessAddressSpaceManager::mapRegion() 한 번으로
        // findGap+Paging::mapPage+장부 등록까지 마친다(RequestIoPermissionHandler,
        // pnp.cpp와 동일한 패턴 - 캐시 속성은 §3.3이 제안한 PAGE_CACHE_DISABLE).
        uint64_t virtAddr = 0;
        const uint64_t mappedLength = kDmaPageSize << order;
        if (!process->addressSpace.mapRegion(mappedLength, PAGE_WRITABLE | PAGE_USER | PAGE_CACHE_DISABLE,
                                              VmaBacking::FixedPhysical, physAddr, &virtAddr)) {
            PageFrameAllocator::freeOrder(physAddr, order);
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }

        // §3.1 4번 - Process::dmaBuffers에 {physAddr, virtAddr, pageCount,
        // handle}을 기록하고 handle을 발급.
        process->dmaBuffers.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        const uint32_t handle = kAllocateDmaHandle(process.get());
        if (handle == 0) {
            process->addressSpace.unmapRegion(virtAddr, mappedLength);
            PageFrameAllocator::freeOrder(physAddr, order);
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }

        Process::DmaBuffer entry;
        entry.physAddr = physAddr;
        entry.virtAddr = virtAddr;
        entry.pageCount = static_cast<uint32_t>(1ULL << order);
        entry.handle = handle;
        entry.used = true;
        process->dmaBuffers.insert(entry);

        args->virtualAddr = virtAddr;
        args->physicalAddr = physAddr;
        args->handle = handle;
        args->error = ChannelError::None;
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

        SharedPtr<Process> process = kProcessFromSubmitterForDma(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }

        const uint32_t handle = args->handle;
        auto* slot =
            process->dmaBuffers.find([handle](const Process::DmaBuffer& e) { return e.handle == handle; });
        if (!slot) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }

        const uint32_t order = kOrderForPageCount(slot->value.pageCount);
        process->addressSpace.unmapRegion(slot->value.virtAddr,
                                           static_cast<uint64_t>(slot->value.pageCount) * kDmaPageSize);
        PageFrameAllocator::freeOrder(slot->value.physAddr, order);
        process->dmaBuffers.erase(slot);

        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

FreeDmaBufferHandler gFreeDmaBufferHandler;

}  // namespace

void DmaBufferService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointAllocDmaBuffer, &gAllocDmaBufferHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointFreeDmaBuffer, &gFreeDmaBufferHandler);
}

}  // namespace kernel
