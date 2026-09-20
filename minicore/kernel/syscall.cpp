#include "syscall.h"

#include "async_task.h"
#include "libkenv/mem.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"
#include "scheduler.h"

namespace {

struct EndpointSlot {
    kernel::AsyncTaskSubjectCode subjectCode = 0;
    bool used = false;
};

// [신규, 2026-09-17, SP-E9B44929 §6 답변 - "그룹별 테이블을 두고,
// 그게 포인터로 기능별 테이블을 가리키게 해. 그룹별 테이블에는
// min/max 필드를 두면 공간을 절약할 수 있어"] 256개 그룹 슬롯은
// 전부 정적으로 두되(포인터+2바이트라 가벼움), 각 그룹의 실제 call
// 테이블은 그 그룹이 실제로 쓰는 [minCall, maxCall] 구간만큼만
// 동적으로 잡는다 - 전체를 65536(그룹×콜) dense 배열로 두는 것보다
// 훨씬 작다(현재 가장 큰 그룹인 Vfs도 14개 call뿐).
struct GroupTable {
    EndpointSlot* calls = nullptr;  // [minCall, maxCall] 구간, calls[call - minCall]로 색인
    kernel::uint8_t minCall = 0;
    kernel::uint8_t maxCall = 0;
};

GroupTable gGroupTables[256];

// [중요] `SelfTerminateHandler`가 `Scheduler::init()`(kmain.cpp) 안에서
// 이 파일의 `registerHandler()`를 부르는데, 그 시점은
// `GenericSlabAllocator::init()`보다 **먼저**다(kmain.cpp 순서 확정 -
// 부팅 극초반, 실측으로 발견: 슬랩 할당자를 여기서 썼다가 초기화 전
// 페이지 폴트로 즉시 패닉했다). 그래서 그룹 call 테이블은 힙이 아니라
// **정적 범프(bump) 풀**에서 잘라 쓴다 - 부팅 시 한 번씩만 등록되고
// (동적 회수 없음) 총 등록 개수가 작아(현재 9개 그룹 합쳐 40개 미만)
// 넉넉히 여유를 둔 정적 배열이면 충분하다.
constexpr kernel::uint32_t kCallSlotPoolCapacity = 1024;  // 실측 후 조정 대상(RM-23F4B687 §4)
EndpointSlot gCallSlotPool[kCallSlotPoolCapacity];
kernel::uint32_t gCallSlotPoolUsed = 0;

// 그룹 테이블이 call을 담을 수 있도록 필요하면 [minCall, maxCall]
// 구간을 확장한다 - 정적 풀에서 새 조각을 잘라 옛 내용을 복사하고
// 옛 조각은 그냥 버려진다(반납 없음, 풀이 넉넉해 문제되지 않음).
bool kEnsureGroupCallSlot(GroupTable& table, kernel::uint8_t call) {
    if (table.calls && call >= table.minCall && call <= table.maxCall) {
        return true;  // 이미 범위 안 - 재할당 불필요
    }
    const kernel::uint8_t newMin = table.calls ? (call < table.minCall ? call : table.minCall) : call;
    const kernel::uint8_t newMax = table.calls ? (call > table.maxCall ? call : table.maxCall) : call;
    const kernel::uint32_t newCount = static_cast<kernel::uint32_t>(newMax) - newMin + 1;
    if (gCallSlotPoolUsed + newCount > kCallSlotPoolCapacity) {
        return false;  // 풀 고갈
    }
    EndpointSlot* newCalls = &gCallSlotPool[gCallSlotPoolUsed];
    gCallSlotPoolUsed += newCount;
    for (kernel::uint32_t i = 0; i < newCount; ++i) {
        newCalls[i] = EndpointSlot{};
    }
    if (table.calls) {
        const kernel::uint32_t oldCount = static_cast<kernel::uint32_t>(table.maxCall) - table.minCall + 1;
        for (kernel::uint32_t i = 0; i < oldCount; ++i) {
            newCalls[(table.minCall + i) - newMin] = table.calls[i];
        }
        // 옛 조각은 반납하지 않는다(정적 풀 - free 개념 없음, 위 주석 참고).
    }
    table.calls = newCalls;
    table.minCall = newMin;
    table.maxCall = newMax;
    return true;
}

}  // namespace

namespace kernel {

// [신규, 2026-09-17, PN-B4987BF6] UserThread::_selfRef 전용 no-op
// 삭제자 - `UserThread::release()`가 여전히 실제 반납(GenericSlabAllocator::
// free)을 직접 담당한다는 기존 계약을 그대로 지키기 위해, `_selfRef`의
// 강한 참조가 0이 되는 순간(release() 안의 reset())엔 아무 것도 하지
// 않는다(syscall.h의 _selfRef 주석 참고 - operator delete/__cxa_atexit
// 스텁과 같은 근거의 "이 훅이 실제로 할 일이 없다" no-op).
void kNoOpReleaseUserThread(UserThread*) {}

// [SP-6BEAE0C1 §5, PN-543C0CE9] Process::allocate()/release()(process.cpp)
// 와 완전히 같은 근거 - UserThread의 모든 필드(Task 상속분 포함)가
// 0/nullptr NSDMI라 memset한 raw 슬랩 메모리가 placement new 없이도
// "방금 생성된" 상태와 동일해진다. 이후 Task::init()이 안전하게
// kernelStackPhys 등을 새로 채운다(기존 값을 읽지 않고 무조건 덮어쓰므로
// 이 부분은 raw 메모리라도 원래 안전했다 - pendingSyscalls처럼 내부
// 포인터를 먼저 걷는 필드만 이 memset이 실제로 막아 주는 대상).
//
// [수정, 2026-09-17, PN-B4987BF6] `_selfRef`를 no-op 삭제자로
// `kMakeShared`해 컨트롤 블록만 곁다리로 붙인다(syscall.h의 `_selfRef`
// 주석 참고) - `EnableSharedFromThis<UserThread>`가 필요로 하는
// `_weakThis`가 이 호출로 채워진다(shared_ptr.h의 kMakeShared가
// `if constexpr` 자동 처리). 실패 시(컨트롤 블록 슬랩 할당 실패)
// UserThread 슬랩 자체도 되돌린다 - 부분 성공 상태를 남기지 않는다.
UserThread* UserThread::allocate() {
    void* raw = GenericSlabAllocator::alloc(sizeof(UserThread));
    if (!raw) {
        return nullptr;
    }
    memset(raw, 0, sizeof(UserThread));
    auto* thread = reinterpret_cast<UserThread*>(raw);
    thread->_selfRef = kMakeShared<UserThread>(thread, &kNoOpReleaseUserThread);
    if (!thread->_selfRef) {
        GenericSlabAllocator::free(raw, sizeof(UserThread));
        return nullptr;
    }
    return thread;
}

// [신규, PN-523B779F] syscall.h의 ensureSelfRef() 문서 주석 참고 -
// allocate()와 정확히 같은 kMakeShared+no-op 삭제자 패턴을 재사용한다.
bool UserThread::ensureSelfRef() {
    if (_selfRef) {
        return true;  // 이미 채워져 있음(allocate() 경로) - 멱등
    }
    _selfRef = kMakeShared<UserThread>(this, &kNoOpReleaseUserThread);
    return static_cast<bool>(_selfRef);
}

void UserThread::release(UserThread* thread) {
    // _selfRef.reset()은 강한 참조 카운트만 0으로 내릴 뿐(no-op
    // 삭제자라 이 시점엔 아무 메모리도 안 건드림) - 실제 반납은 항상
    // 이 함수의 GenericSlabAllocator::free()가 담당한다는 기존 계약
    // 그대로다(syscall.h의 _selfRef 주석 참고). 순서가 중요하지 않다 -
    // reset() 다음 줄에서 thread가 가리키는 메모리를 또 만지지 않는다.
    thread->_selfRef.reset();
    // [신규, 2026-09-19, PN-8726CDBD] fpuContext(UniquePtr)는 아래
    // GenericSlabAllocator::free()가 raw 메모리를 그냥 반납할 뿐
    // 소멸자를 부르지 않으므로(이 struct 전체가 memset(0)+init()
    // 관례, task.h 문서 주석 참고), 여기서 명시적으로 reset()해
    // 할당돼 있었을 TaskFpuContext를 먼저 반납하지 않으면 그 512바이트
    // 슬랩 블록이 그대로 샌다.
    thread->fpuContext.reset();
    GenericSlabAllocator::free(thread, sizeof(UserThread));
}

bool SyscallRegistry::registerHandler(SyscallEndpointId endpointId, AsyncTaskHandler* handler) {
    if (endpointId & kSyscallReservedMask) {
        return false;  // 비트 31:16은 반드시 0(SP-E9B44929)
    }
    const uint8_t group = kSyscallGroupOf(endpointId);
    const uint8_t call = kSyscallCallOf(endpointId);
    GroupTable& table = gGroupTables[group];
    if (table.calls && call >= table.minCall && call <= table.maxCall &&
        table.calls[call - table.minCall].used) {
        return false;  // 이미 채워진 슬롯(설계 실수 조기 발견용, 기존 계약 그대로)
    }
    if (!kEnsureGroupCallSlot(table, call)) {
        return false;
    }
    EndpointSlot& slot = table.calls[call - table.minCall];
    slot.subjectCode = AsyncCallbackRegistry::registerHandler(handler);
    slot.used = true;
    return true;
}

bool SyscallRegistry::resolveSubjectCode(SyscallEndpointId endpointId, AsyncTaskSubjectCode* outSubjectCode) {
    if (endpointId & kSyscallReservedMask) {
        return false;
    }
    const uint8_t group = kSyscallGroupOf(endpointId);
    const uint8_t call = kSyscallCallOf(endpointId);
    GroupTable& table = gGroupTables[group];
    if (!table.calls || call < table.minCall || call > table.maxCall) {
        return false;
    }
    const EndpointSlot& slot = table.calls[call - table.minCall];
    if (!slot.used) {
        return false;
    }
    *outSubjectCode = slot.subjectCode;
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
    // 소비/반납한다. preemptive=true - [PN-4FA5F13B 근본 원인 수정]
    // 이 제출자는 곧 waitForAnyOf()->parkCurrent()로 실제 블로킹할 수
    // 있다 - IPI로 강제 드레인해야 이 코어에 계속 Ready인 다른 Task가
    // 있어도(예: 방금 SpawnProcess로 뜬, syscall을 안 쓰는 CPU-bound
    // 자식) drainOnce()가 idle 분기 도달 실패로 굶지 않는다(async_task.h
    // submit() 문서 참고).
    AsyncTask* task = AsyncTask::submit(subjectCode, 0, args, /*autoFree=*/false, /*preemptive=*/true);
    if (!task) {
        return 0;  // Slab 고갈 등
    }
    // [신규, 2026-09-17, PN-DB5153B6] 지금(=제출 시점)이 `Scheduler::
    // currentTask()`가 정확한 마지막 순간이다 - onExec()은 나중에
    // AsyncReactor가 자기 스택 위에서 실행하므로 그 안에서는 이미
    // 늦다(async_task.h의 `submitterTask` 필드 주석 참고).
    // [갱신, 2026-09-20, PN-C536F352] TaskOwnerRef::capture() - 바로
    // 이 지점이 "진짜 제출 컨텍스트"라는 사실 자체가 TaskOwnerRef의
    // 핵심 불변조건(제출 시점에만 캡처, 지연 실행 컨텍스트에서는
    // 절대 재호출 금지)이다.
    task->submitterTask = TaskOwnerRef::capture(self->weakAsTask());

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

void Syscall::submitDetached(SyscallEndpointId endpointId, void* args) {
    AsyncTaskSubjectCode subjectCode = 0;
    if (!SyscallRegistry::resolveSubjectCode(endpointId, &subjectCode)) {
        return;
    }
    // submit()과 달리 pendingSyscalls 부기가 전혀 없다 - autoFree=true라
    // 리액터가 onExec 완료 직후(reactorTaskEntry, async_task.cpp) 이
    // AsyncTask 구조체/전용 스택을 스스로 반납한다.
    AsyncTask::submit(subjectCode, 0, args, /*autoFree=*/true);
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
                // [신규, 2026-09-18, PN-B5C2845A] `Cancelled`도 "끝남"
                // 으로 인식한다 - 다른 프로세스의 Kill이 이 토큰을
                // 취소시켰을 수 있다(`Scheduler::cancelPendingSyscalls`).
                // Completed와 구분할 필요가 없어(호출부는 결국 실패로
                // 다뤄야 함) Failed와 같은 outcome으로 매핑한다 -
                // 새 MultiWaitOutcome 값을 추가하지 않는다.
                if (task->state == AsyncTaskState::Completed || task->state == AsyncTaskState::Failed ||
                    task->state == AsyncTaskState::Cancelled) {
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
                    reinterpret_cast<AsyncTask*>(slot->value.token)->waitingTask = self->weakAsTask();
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
