#ifndef MINICORE_LIBS_LIBSWAPFS_SWAPFS_IO_H
#define MINICORE_LIBS_LIBSWAPFS_SWAPFS_IO_H

#include "async_task.h"
#include "block_device.h"
#include "swap_backend.h"

// [준비 작업, PN-4859FDE9] swapfs.cpp의 SwapfsBackend::writeSlot()/
// readSlot()(동기 - 내부적으로 BlockDevice::writeBlocks()/readBlocks()
// 라는 "제출 후 그 자리에서 완료까지 대기"하는 편의 래퍼를 씀)와 정확히
// 같은 슬롯->LBA 산출 공식을 쓰되, 전송 자체는 코루틴 onExec() 안에서
// co_await kernel::AsyncTaskCoroAwaiter(...)로 논블로킹 수행하는 별도
// 경로다. 페이지 프레임 회수 스캔 콜백(page_frame_allocator.cpp의
// kReclaimScanCallback - DelayedExecutionQueue::pump() 안, 즉
// AsyncReactor::drainOnce() 자신의 C++ 콜스택 위에서 평범한 함수
// 포인터로 불림)처럼 코루틴이 아닌 컨텍스트는 어떤 블로킹 대기
// (AsyncTaskAwaiter::await() 포함 - Scheduler::yieldCurrent()를 호출해
// 진짜 kernel::Task 컨텍스트를 요구함)도 안전하게 쓸 수 없어, 그
// 동기 writeSlot()/readSlot()을 그 컨텍스트에서 그대로 재사용할 수
// 없다(SwapBackend 인터페이스 자체는 그대로 - 이건 그 위에 얹는
// 별도의 비동기 소비 경로일 뿐, SP-D02C4A73을 수정하지 않는다).
//
// 이 두 핸들러는 순수한 "슬롯 하나 읽기/쓰기" 원시 동작만 한다 -
// 회수 스캔의 "쓰기 완료 후 물리 프레임 반납" 같은 호출부별 후속
// 조치는 의도적으로 포함하지 않는다(제출자가 완료를 관측한 뒤 직접
// 처리) - 페이지 폴트 인(읽기) 경로와 회수 스캔(쓰기) 경로 양쪽이
// 그대로 공유할 수 있는 최소 단위로 유지하기 위함.
namespace fs {

// SwapWriteHandler/SwapReadHandler onExec()의 인자 - 호출부가
// GenericSlabAllocator로 할당해 AsyncTask::submit()에 넘기고, 완료
// (Completed/Failed/Cancelled 무엇이든) 후 이 핸들러 자신이 해제한다
// (AhciCommandHandler/AhciCommandArgs와 동일한 소유권 관례, ahci.cpp
// 참고). outResult는 args와 별개로 호출부가 소유 - 이 AsyncTask가
// 끝날 때까지(그리고 그 결과를 읽을 때까지) 호출부가 살려 둬야 한다
// (args 자신은 그 전에 이미 해제될 수 있으므로 절대 args 안에 결과를
// 담지 않는다).
struct SwapIoArgs {
    BlockDevice* device = nullptr;
    SwapSlot slot = 0;
    void* page = nullptr;  // 정확히 kSwapPageSize(4096)바이트 - 쓰기는 원본, 읽기는 목적지
    BlockIoResult* outResult = nullptr;
};

class SwapWriteHandler : public kernel::AsyncTaskHandler {
public:
    kernel::AsyncExecCoro onExec(kernel::AsyncTask* task, void* argsRaw) override;
    void onFailure(kernel::AsyncTask* task) override;
    void onCancel(kernel::AsyncTask* task, void* argsRaw) override;
};

class SwapReadHandler : public kernel::AsyncTaskHandler {
public:
    kernel::AsyncExecCoro onExec(kernel::AsyncTask* task, void* argsRaw) override;
    void onFailure(kernel::AsyncTask* task) override;
    void onCancel(kernel::AsyncTask* task, void* argsRaw) override;
};

// 부팅 시(또는 첫 사용 시) 1회 등록 후 subjectCode 반환 - ahci.cpp의
// kEnsureAhciCommandHandlerRegistered()와 동일한 지연 등록 관례.
kernel::AsyncTaskSubjectCode kEnsureSwapWriteHandlerRegistered();
kernel::AsyncTaskSubjectCode kEnsureSwapReadHandlerRegistered();

}  // namespace fs

#endif  // MINICORE_LIBS_LIBSWAPFS_SWAPFS_IO_H
