#include "swapfs_io.h"

#include "libkmm/slab.h"
#include "swapfs.h"  // kSwapPageSize

namespace fs {

namespace {

// SwapfsBackend::writeSlot()/readSlot()(swapfs.cpp)과 동일한 슬롯->LBA
// 산출 공식 - 두 곳 다 바꿀 땐 반드시 함께 맞춘다.
kernel::AsyncTask* kSubmitSwapTransfer(BlockDevice* device, SwapSlot slot, void* page, bool isWrite,
                                        BlockIoResult* outResult) {
    const kernel::uint32_t blockSize = device->blockSize();
    if (blockSize == 0 || kSwapPageSize % blockSize != 0) {
        return nullptr;
    }
    const kernel::uint32_t blocksPerPage = static_cast<kernel::uint32_t>(kSwapPageSize / blockSize);
    const kernel::uint64_t lba = (slot * kSwapPageSize) / blockSize;
    if (isWrite) {
        return device->submitWriteBlocks(lba, page, blocksPerPage, outResult);
    }
    return device->submitReadBlocks(lba, page, blocksPerPage, outResult);
}

}  // namespace

kernel::AsyncExecCoro SwapWriteHandler::onExec(kernel::AsyncTask*, void* argsRaw) {
    auto* args = static_cast<SwapIoArgs*>(argsRaw);
    kernel::AsyncTask* ioTask =
        kSubmitSwapTransfer(args->device, args->slot, args->page, /*isWrite=*/true, args->outResult);
    if (ioTask) {
        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
    } else {
        args->outResult->ok = false;  // 제출 자체가 불가능(슬롯 고갈 등) - submitWriteBlocks 계약상 outResult 미변경이므로 여기서 직접 채운다
    }
    kernel::GenericSlabAllocator::free(args, sizeof(SwapIoArgs));
    co_return;
}

void SwapWriteHandler::onFailure(kernel::AsyncTask*) {}

void SwapWriteHandler::onCancel(kernel::AsyncTask*, void* argsRaw) {
    // [ahci.cpp AhciCommandHandler::onCancel과 동일 관례] outResult는
    // 호출부가 이미 사라졌을 수 있어 건드리지 않는다 - args만 반납.
    auto* args = static_cast<SwapIoArgs*>(argsRaw);
    kernel::GenericSlabAllocator::free(args, sizeof(SwapIoArgs));
}

kernel::AsyncExecCoro SwapReadHandler::onExec(kernel::AsyncTask*, void* argsRaw) {
    auto* args = static_cast<SwapIoArgs*>(argsRaw);
    kernel::AsyncTask* ioTask =
        kSubmitSwapTransfer(args->device, args->slot, args->page, /*isWrite=*/false, args->outResult);
    if (ioTask) {
        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
    } else {
        args->outResult->ok = false;
    }
    kernel::GenericSlabAllocator::free(args, sizeof(SwapIoArgs));
    co_return;
}

void SwapReadHandler::onFailure(kernel::AsyncTask*) {}

void SwapReadHandler::onCancel(kernel::AsyncTask*, void* argsRaw) {
    auto* args = static_cast<SwapIoArgs*>(argsRaw);
    kernel::GenericSlabAllocator::free(args, sizeof(SwapIoArgs));
}

namespace {

SwapWriteHandler gSwapWriteHandler;
kernel::AsyncTaskSubjectCode gSwapWriteSubjectCode = 0;
bool gSwapWriteHandlerRegistered = false;

SwapReadHandler gSwapReadHandler;
kernel::AsyncTaskSubjectCode gSwapReadSubjectCode = 0;
bool gSwapReadHandlerRegistered = false;

}  // namespace

kernel::AsyncTaskSubjectCode kEnsureSwapWriteHandlerRegistered() {
    if (!gSwapWriteHandlerRegistered) {
        gSwapWriteSubjectCode = kernel::AsyncCallbackRegistry::registerHandler(&gSwapWriteHandler);
        gSwapWriteHandlerRegistered = true;
    }
    return gSwapWriteSubjectCode;
}

kernel::AsyncTaskSubjectCode kEnsureSwapReadHandlerRegistered() {
    if (!gSwapReadHandlerRegistered) {
        gSwapReadSubjectCode = kernel::AsyncCallbackRegistry::registerHandler(&gSwapReadHandler);
        gSwapReadHandlerRegistered = true;
    }
    return gSwapReadSubjectCode;
}

}  // namespace fs
