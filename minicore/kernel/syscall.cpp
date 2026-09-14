#include "syscall.h"

#include "async_task.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"
#include "scheduler.h"

namespace {

constexpr kernel::uint32_t kMaxSyscallEndpoints = 256;  // v1 상한 - 필요해지면 늘림

struct EndpointSlot {
    kernel::AsyncTaskSubjectCode subjectCode = 0;
    bool used = false;
};

EndpointSlot gEndpointSlots[kMaxSyscallEndpoints];

}  // namespace

namespace kernel {

bool SyscallRegistry::registerHandler(SyscallEndpointId endpointId, AsyncTaskHandler* handler) {
    if (endpointId >= kMaxSyscallEndpoints || gEndpointSlots[endpointId].used) {
        return false;
    }
    gEndpointSlots[endpointId].subjectCode = AsyncCallbackRegistry::registerHandler(handler);
    gEndpointSlots[endpointId].used = true;
    return true;
}

bool SyscallRegistry::resolveSubjectCode(SyscallEndpointId endpointId, AsyncTaskSubjectCode* outSubjectCode) {
    if (endpointId >= kMaxSyscallEndpoints || !gEndpointSlots[endpointId].used) {
        return false;
    }
    *outSubjectCode = gEndpointSlots[endpointId].subjectCode;
    return true;
}

AsyncTaskManageCode Syscall::submit(SyscallEndpointId endpointId, void* args) {
    AsyncTaskSubjectCode subjectCode = 0;
    if (!SyscallRegistry::resolveSubjectCode(endpointId, &subjectCode)) {
        return 0;  // 등록 안 된 endpoint - 즉시 실패(비블로킹)
    }
    // autoFree=false - 결과를 나중에 wait()이 직접 소비/반납한다.
    AsyncTask* task = AsyncTask::submit(subjectCode, 0, args, /*autoFree=*/false);
    if (!task) {
        return 0;  // Slab 고갈 등
    }

    auto* self = static_cast<UserThread*>(Scheduler::currentTask());
    self->pendingSyscall.endpoint = endpointId;
    self->pendingSyscall.token = reinterpret_cast<AsyncTaskManageCode>(task);
    self->pendingSyscall.valid = true;
    return self->pendingSyscall.token;
}

bool Syscall::wait(AsyncTaskManageCode token) {
    auto* self = static_cast<UserThread*>(Scheduler::currentTask());
    if (!self->pendingSyscall.valid || self->pendingSyscall.token != token) {
        return false;  // 위조/타인 토큰이거나 이미 소비된 토큰 - 전역 검색 없이 자기 필드와만 비교
    }
    auto* task = reinterpret_cast<AsyncTask*>(token);

    for (;;) {
        {
            // 이 스코프 안에서는 이 코어가 선점되지 않는다 - "아직 완료
            // 안 됨을 확인하고 waitingTask를 등록하는" 사이에 스케줄러
            // 틱이 끼어들어 리액터가 먼저 이 AsyncTask를 완료시켜 버리면
            // (그 시점엔 waitingTask가 아직 비어 있어 아무도 깨우지
            // 않음), 이후 우리가 파킹돼도 영원히 못 깨어난다 - 그 경쟁을
            // 막는다.
            PreemptionGuard guard;
            if (task->state == AsyncTaskState::Completed || task->state == AsyncTaskState::Failed) {
                const bool ok = task->state == AsyncTaskState::Completed;
                self->pendingSyscall.valid = false;  // 토큰 소비(재사용/이중 반납 방지)
                GenericSlabAllocator::free(reinterpret_cast<void*>(task->stackBase), kAsyncTaskStackSize);
                GenericSlabAllocator::free(task, sizeof(AsyncTask));
                return ok;
            }
            task->waitingTask = self;
        }
        // 여기서 어떤 이유로든(설계자 지시 2번 - 아직 이 프로젝트에
        // 설계되지 않은 시그널 등) 풀려도, 유저랜드가 그냥 같은
        // token으로 wait()을 다시 부르면 된다 - 위 pendingSyscall 검사가
        // 여전히 유효한 토큰임을 확인해 주므로 별도 재합류 API가
        // 필요 없다(이 for 루프 자체가 그 재합류와 동일한 코드 경로).
        Scheduler::parkCurrent();
    }
}

}  // namespace kernel
