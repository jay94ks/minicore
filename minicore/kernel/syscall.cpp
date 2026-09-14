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
    auto* self = static_cast<UserThread*>(Scheduler::currentTask());
    self->pendingSyscalls.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);

    // 이 스코프 전체를 선점 금지로 감싼다 - AsyncTask::submit()은 내부에서
    // 곧바로 AsyncReactor::submitCompletion()을 불러 이 AsyncTask를 이
    // 코어의 실행 큐에 올린다(리액터가 파킹돼 있었다면 깨우기까지 함).
    // 그 직후 pendingSyscalls.insert()가 실패하면(목록 청크용 Slab
    // 고갈) autoFree를 뒤늦게 true로 되돌려 리액터가 대신 정리하게
    // 해야 하는데, 그 사이 리액터가 먼저 끼어들어 이 AsyncTask를
    // 완료시켜 버리면(그때는 아직 autoFree=false라 아무도 반납 안 함)
    // 영구히 샌다 - 선점을 막아 "제출 -> (실패 시) autoFree 되돌리기"
    // 구간 전체를 리액터가 절대 끼어들 수 없는 원자적 구간으로 만든다.
    PreemptionGuard guard;

    // autoFree=false - 결과를 나중에 wait()/waitForAnyOf()가 직접
    // 소비/반납한다.
    AsyncTask* task = AsyncTask::submit(subjectCode, 0, args, /*autoFree=*/false);
    if (!task) {
        return 0;  // Slab 고갈 등
    }

    const AsyncTaskManageCode token = reinterpret_cast<AsyncTaskManageCode>(task);
    if (!self->pendingSyscalls.insert(UserThread::PendingSyscall{endpointId, token})) {
        // 극히 드문 이중 고갈(AsyncTask 구조체/스택은 확보됐는데 목록
        // 슬롯용 청크는 못 만든 경우) - 이미 리액터 실행 큐에 올라간
        // AsyncTask를 되돌릴 수 없으니, autoFree를 true로 되돌려
        // 완료 시 리액터가 대신 반납하게 한다(위 PreemptionGuard가
        // 그때까지 리액터의 실행 자체를 막아 안전하다).
        task->autoFree = true;
        return 0;
    }
    return token;
}

bool Syscall::wait(AsyncTaskManageCode token) {
    return waitForAnyOf(&token, 1).outcome == MultiWaitOutcome::Completed;
}

Syscall::MultiWaitResult Syscall::waitForMultipleSyscall(const AsyncTaskManageCode* tokens, uint32_t count) {
    return waitForAnyOf(tokens, count);
}

Syscall::MultiWaitResult Syscall::waitAnyForMultipleSyscall(const AsyncTaskManageCode* tokens, uint32_t count) {
    return waitForAnyOf(tokens, count);
}

Syscall::MultiWaitResult Syscall::waitForAnyOf(const AsyncTaskManageCode* tokens, uint32_t count) {
    auto* self = static_cast<UserThread*>(Scheduler::currentTask());
    if (count == 0) {
        return {0, MultiWaitOutcome::Invalid};
    }

    for (;;) {
        MultiWaitResult result{0, MultiWaitOutcome::Invalid};
        decltype(self->pendingSyscalls)::Slot* readySlot = nullptr;
        AsyncTask* readyTask = nullptr;
        bool found = false;

        {
            // 이 스코프 안에서는 이 코어가 선점되지 않는다 - "아직 완료
            // 안 됨을 확인하고 waitingTask를 등록하는" 사이에 스케줄러
            // 틱이 끼어들어 리액터가 먼저 이 AsyncTask들 중 하나를
            // 완료시켜 버리면(그 시점엔 waitingTask가 아직 비어 있어
            // 아무도 깨우지 않음), 이후 우리가 파킹돼도 영원히 못
            // 깨어난다 - 그 경쟁을 막는다(기존 wait()과 동일한 이유).
            PreemptionGuard guard;

            // 1차: 넘겨받은 토큰들 중 이미 끝났거나(Completed/Failed)
            // 유효하지 않은(자기 소유가 아니거나 이미 소비된) 게
            // 있는지 먼저 찾는다 - 있으면 블로킹 없이 그 하나만 소비.
            for (uint32_t i = 0; i < count && !found; ++i) {
                auto* slot = self->pendingSyscalls.find(
                    [&](const UserThread::PendingSyscall& p) { return p.token == tokens[i]; });
                if (!slot) {
                    result = {tokens[i], MultiWaitOutcome::Invalid};
                    found = true;
                    break;
                }
                auto* task = reinterpret_cast<AsyncTask*>(slot->value.token);
                if (task->state == AsyncTaskState::Completed || task->state == AsyncTaskState::Failed) {
                    result = {tokens[i], task->state == AsyncTaskState::Completed ? MultiWaitOutcome::Completed
                                                                                   : MultiWaitOutcome::Failed};
                    readySlot = slot;
                    readyTask = task;
                    found = true;
                }
            }

            // 2차: 아무것도 안 끝났으면, 이 스레드를 tokens 전부에
            // 등록해 둔다 - 그중 먼저 끝나는 아무 하나가 우리를 깨운다.
            if (!found) {
                for (uint32_t i = 0; i < count; ++i) {
                    auto* slot = self->pendingSyscalls.find(
                        [&](const UserThread::PendingSyscall& p) { return p.token == tokens[i]; });
                    reinterpret_cast<AsyncTask*>(slot->value.token)->waitingTask = self;
                }
            }
        }

        if (found) {
            if (readySlot) {
                self->pendingSyscalls.erase(readySlot);
            }
            if (readyTask) {
                GenericSlabAllocator::free(reinterpret_cast<void*>(readyTask->stackBase), kAsyncTaskStackSize);
                GenericSlabAllocator::free(readyTask, sizeof(AsyncTask));
            }
            return result;
        }

        // **실측으로 발견한 경쟁(2026-09-14, Channel IPC 스트레스
        // 테스트, 기존 wait()과 동일한 이유)**: 위 PreemptionGuard
        // 스코프가 끝난 시점과 실제로 Scheduler::parkCurrent()에
        // 진입하는 시점 사이에 스케줄러 틱이 끼어들면 이중 스케줄링이
        // 될 수 있다 - 여기서 미리 거는 cli는 parkCurrent() 안의
        // cli와 중복(멱등)이라 무해하다.
        asm volatile("cli");
        // 여기서 풀려도 유저랜드가 그냥 같은 tokens로 다시 부르면
        // 된다 - 이 for 루프 자체가 그 재합류와 동일한 코드 경로다.
        Scheduler::parkCurrent();
    }
}

}  // namespace kernel
