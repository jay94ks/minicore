#include "block_device.h"

#include "async_task.h"

namespace fs {

void BlockDevice::submitReadBlocksBatch(const BlockReadRequest* requests, kernel::uint32_t count,
                                         kernel::AsyncTask** outTasks, BlockIoResult* outResults) {
    for (kernel::uint32_t i = 0; i < count; ++i) {
        outResults[i] = BlockIoResult{};
        outTasks[i] = submitReadBlocks(requests[i].lba, requests[i].buffer, requests[i].count, &outResults[i]);
    }
}

void BlockDevice::submitWriteBlocksBatch(const BlockWriteRequest* requests, kernel::uint32_t count,
                                          kernel::AsyncTask** outTasks, BlockIoResult* outResults) {
    for (kernel::uint32_t i = 0; i < count; ++i) {
        outResults[i] = BlockIoResult{};
        outTasks[i] = submitWriteBlocks(requests[i].lba, requests[i].buffer, requests[i].count, &outResults[i]);
    }
}

bool BlockDevice::readBlocks(kernel::uint64_t lba, kernel::uint32_t count, void* buf) {
    BlockIoResult result;
    kernel::AsyncTask* task = submitReadBlocks(lba, buf, count, &result);
    if (!task) {
        return false;
    }
    kernel::AsyncTaskWaitGroup group;
    group.add(task);
    group.waitAll();
    return result.ok;
}

bool BlockDevice::writeBlocks(kernel::uint64_t lba, kernel::uint32_t count, const void* buf) {
    BlockIoResult result;
    kernel::AsyncTask* task = submitWriteBlocks(lba, buf, count, &result);
    if (!task) {
        return false;
    }
    kernel::AsyncTaskWaitGroup group;
    group.add(task);
    group.waitAll();
    return result.ok;
}

}  // namespace fs
