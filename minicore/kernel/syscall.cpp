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

namespace {

// [신규, 2026-09-27, PN-395F4D89 방향 B] wait()/waitForMultipleSyscall()/
// waitAnyForMultipleSyscall()/재개 트램폴린이 공유하는 "한 번 확인"
// 로직 - 원래 waitForAnyOf()의 for(;;) 루프 1차/2차 확인 부분을
// 순서/타이밍 전부 그대로 옮겨 왔다(로직 자체는 전혀 안 바뀜 -
// PreemptionGuard 스코프, erase/free를 그 스코프 **밖**에서 하는
// 순서까지 원본과 동일). 재개 트램폴린에서도 재사용하기 위해 분리.
struct PendingWaitCheckOutcome {
    bool found = false;
    Syscall::MultiWaitResult result{};
};

PendingWaitCheckOutcome kCheckPendingWaitOnce(UserThread* self, const AsyncTaskManageCode* tokens, uint32_t count) {
    PendingWaitCheckOutcome outcome;
    decltype(self->pendingSyscalls)::Slot* readySlot = nullptr;
    AsyncTask* readyTask = nullptr;

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
        for (uint32_t i = 0; i < count && !outcome.found; ++i) {
            auto* slot = self->pendingSyscalls.find(
                [&](const UserThread::PendingSyscall& p) { return p.token == tokens[i]; });
            if (!slot) {
                outcome.result = {tokens[i], Syscall::MultiWaitOutcome::Invalid};
                outcome.found = true;
                break;
            }
            auto* task = reinterpret_cast<AsyncTask*>(slot->value.token);
            // [신규, 2026-09-18, PN-B5C2845A] `Cancelled`도 "끝남"으로
            // 인식한다 - 다른 프로세스의 Kill이 이 토큰을 취소시켰을
            // 수 있다(`Scheduler::cancelPendingSyscalls`). Completed와
            // 구분할 필요가 없어(호출부는 결국 실패로 다뤄야 함) Failed
            // 와 같은 outcome으로 매핑한다 - 새 MultiWaitOutcome 값을
            // 추가하지 않는다.
            if (task->state == AsyncTaskState::Completed || task->state == AsyncTaskState::Failed ||
                task->state == AsyncTaskState::Cancelled) {
                outcome.result = {tokens[i], task->state == AsyncTaskState::Completed
                                                  ? Syscall::MultiWaitOutcome::Completed
                                                  : Syscall::MultiWaitOutcome::Failed};
                readySlot = slot;
                readyTask = task;
                outcome.found = true;
            }
        }

        // 2차: 아무것도 안 끝났으면, 이 스레드를 tokens 전부에 등록해
        // 둔다 - 그중 먼저 끝나는 아무 하나가 우리를 깨운다.
        if (!outcome.found) {
            for (uint32_t i = 0; i < count; ++i) {
                auto* slot = self->pendingSyscalls.find(
                    [&](const UserThread::PendingSyscall& p) { return p.token == tokens[i]; });
                reinterpret_cast<AsyncTask*>(slot->value.token)->waitingTask = self->weakAsTask();
            }
        }
    }

    if (readySlot) {
        self->pendingSyscalls.erase(readySlot);
    }
    if (readyTask) {
        GenericSlabAllocator::free(reinterpret_cast<void*>(readyTask->stackBase), kAsyncTaskStackSize);
        if (readyTask->tcb) {
            GenericSlabAllocator::free(readyTask->tcb, sizeof(TaskTcb));  // PN-81E49523 2단계 - stackBase와 별도 할당
        }
        GenericSlabAllocator::free(readyTask, sizeof(AsyncTask));
    }
    return outcome;
}

}  // namespace

extern "C" [[noreturn]] void kSyscallResumeTrampolineBody();

namespace {

// [신규, 2026-09-27, PN-395F4D89 방향 B, DC-D8951156] `Syscall::
// waitForAnyOf()`가 `int 0x80` 경로에서 실제로 블로킹을 결정했을 때
// 부르는 유일한 진입점 - 근본 수정의 핵심. `frame`(스왑 이전 진짜
// InterruptFrame)을 `self->pendingSyscallReturnFrame`에 스냅샷해 둔
// 뒤, `Task::tcb` 자신은 `kSyscallResumeTrampolineBody`를 가리키는
// 합성 프레임으로 덮어써 "그 지점에서, 이 스레드 전용 커널 스택
// (kernelStackTop) 위에서" 재개되게 한다 - `Scheduler::
// kSyncRsp0ForDispatch()`가 매 디스패치마다 TSS.RSP0을 이 값으로
// 이미 맞춰 두므로(scheduler.cpp), `int 0x80` 트랩 자체도 원래 이
// 스택 위로 들어왔었다(공유 스크래치로의 추가 스왑은 isr_common_stub
// 이 그 *이후에* 한 것). 이 스택은 이 스레드가 다시 뽑힐 때까지
// 아무도 건드리지 않으므로 - DC-D8951156이 확정한 근본 원인(범용
// kContextSwitch가 코어 공유 gInterruptDispatchStacks 위의 한 지점을
// 재개 지점으로 저장)이 여기서는 아예 발생하지 않는다.
[[noreturn]] void kParkForSyscallWaitAndResumeLater(UserThread* self, InterruptFrame* frame) {
    self->pendingSyscallReturnFrame = *frame;

    InterruptFrame resumeFrame{};
    resumeFrame.rip = reinterpret_cast<uint64_t>(&kSyscallResumeTrampolineBody);
    resumeFrame.cs = 0x08;        // kGdtKernelCodeSelector(task.cpp의 Task::init()과 동일한 상수)
    resumeFrame.rflags = 0x202;   // IF=1 - kTaskStartTrampoline 착지 관례와 동일(task.cpp)
    resumeFrame.rspOld = self->kernelStackTop;
    resumeFrame.ssOld = 0x10;     // kGdtKernelDataSelector

    Scheduler::parkWithSyntheticFrame(self, resumeFrame);
}

}  // namespace

bool Syscall::wait(AsyncTaskManageCode token, InterruptFrame* frame) {
    auto* self = static_cast<UserThread*>(Scheduler::currentTask());
    // [신규, PN-395F4D89 방향 B] 이 값 자신(self가 소유)의 주소를
    // 넘긴다 - `&token`(이 함수의 지역 변수)을 그대로 넘기면, 파킹
    // 후 재개 트램폴린이 원래 C++ 콜스택이 사라진 뒤 그 주소를 다시
    // 읽으려 할 때 댕글링이 된다(syscall.h의 pendingWaitSingleToken
    // 문서 주석 참고).
    self->pendingWaitSingleToken = token;
    self->pendingWaitAnyOfArgs = nullptr;  // 방어적 - Wait은 write-back할 유저 구조체가 없음
    return waitForAnyOf(&self->pendingWaitSingleToken, 1, frame).outcome == MultiWaitOutcome::Completed;
}

Syscall::MultiWaitResult Syscall::waitForMultipleSyscall(const AsyncTaskManageCode* tokens, uint32_t count,
                                                          InterruptFrame* frame) {
    return waitForAnyOf(tokens, count, frame);
}

Syscall::MultiWaitResult Syscall::waitAnyForMultipleSyscall(WaitAnyOfSyscallArgs* args, InterruptFrame* frame) {
    auto* self = static_cast<UserThread*>(Scheduler::currentTask());
    self->pendingWaitAnyOfArgs = args;
    const MultiWaitResult result = waitForAnyOf(args->tokens, args->count, frame);
    // 이 지점에 도달했다는 건 파킹 없이(또는 syscall 빠른 경로로
    // 파킹 후 정상 재개돼) 곧장 반환하는 경우뿐이다 - int 0x80 경로가
    // 실제로 파킹했다면 kParkForSyscallWaitAndResumeLater가
    // [[noreturn]]이라 이 줄로 다시 돌아오지 않는다(그 경우
    // pendingWaitAnyOfArgs는 재개 트램폴린이 직접 소비/nullptr로
    // 되돌린다). 여기서도 되돌려야 다음 서로 무관한 syscall에 이
    // 상태가 새어나가지 않는다.
    self->pendingWaitAnyOfArgs = nullptr;
    return result;
}

Syscall::MultiWaitResult Syscall::waitForAnyOf(const AsyncTaskManageCode* tokens, uint32_t count,
                                                InterruptFrame* frame) {
    auto* self = static_cast<UserThread*>(Scheduler::currentTask());
    if (count == 0) {
        return {0, MultiWaitOutcome::Invalid};
    }

    for (;;) {
        PendingWaitCheckOutcome outcome = kCheckPendingWaitOnce(self, tokens, count);
        if (outcome.found) {
            return outcome.result;
        }

        // **실측으로 발견한 경쟁(2026-09-14, Channel IPC 스트레스
        // 테스트, 기존 wait()과 동일한 이유)**: 아래 두 분기 모두
        // "확인 완료 ~ 실제 파킹" 사이에 스케줄러 틱이 끼어들면 이중
        // 스케줄링이 될 수 있다 - 여기서 미리 거는 cli는 parkCurrent()
        // /parkWithSyntheticFrame() 안의 cli와 중복(멱등)이라 무해하다.
        asm volatile("cli");

        if (!frame) {
            // [신규, 2026-09-27, PN-395F4D89 방향 B] `syscall` 빠른
            // 경로(레지스터 기반, `int 0x80`이 아님) - isr_common_stub
            // 의 공유 스크래치 스택(gInterruptDispatchStacks)을 애초에
            // 안 타므로(syscall_fastpath.cpp 자체 문서 주석, DC-D8951156
            // 배경 절 3번 확인) 이 경로의 `Scheduler::parkCurrent()`는
            // 이미 안전하다 - 이 스레드 자신의 전용 커널 스택
            // (TSS.RSP0=kernelStackTop) 위에서 계속 실행 중이기 때문.
            // 기존 동작 그대로 둔다(DC-D8951156 수정 범위 밖).
            Scheduler::parkCurrent();
            continue;
        }

        // [신규, 2026-09-27, PN-395F4D89 방향 B, DC-D8951156] `int 0x80`
        // 경로 - 여기부터 근본 수정 대상. 이 호출 시점의 C++ 콜스택은
        // 아직 `gInterruptDispatchStacks[coreIndex]`(스왑된 공유
        // 스크래치) 위에 있다 - 범용 `parkCurrent()`로 "지금 여기"를
        // 재개 지점으로 저장하면 그 스택이 다음 인터럽트에 덮어써진다
        // (근본 원인, PN-395F4D89 11회차). 워치셋은 이미 durable한
        // 위치(self 소유 또는 유저 메모리, 위 syscall.h 문서 참고)를
        // 가리키고 있으므로 그대로 self에 옮겨 담고,
        // `kParkForSyscallWaitAndResumeLater`(위)로 넘긴다 - 그 함수가
        // [[noreturn]]이라 이 지점으로 다시는 안 돌아온다.
        self->pendingWaitTokens = tokens;
        self->pendingWaitCount = count;
        kParkForSyscallWaitAndResumeLater(self, frame);
        __builtin_unreachable();
    }
}

}  // namespace kernel

// [신규, 2026-09-27, PN-395F4D89 방향 B, DC-D8951156] `kTaskOnFallingToEnd`/
// `kThreadOnFallingToEnd`(scheduler.cpp)와 동일한 관례로 `namespace
// kernel` 밖에 둔다(트램폴린이 순수 `rip` 주소로만 진입해 심볼을
// 찾으므로 extern "C" 링키지 이름이 이 위치와 무관하긴 하지만, 이
// 코드베이스가 이미 그런 진입점들을 전부 여기 두고 있다). 이 함수는
// `kernel::Scheduler::parkWithSyntheticFrame()`이 심어 둔 합성
// 프레임의 착지점 - 항상 그 호출 시점에 지정했던
// `self->kernelStackTop`(이 스레드 전용 커널 스택) 위에서, 이 스레드로
// CR3/TSS.RSP0이 이미 동기화된 채로 실행된다(어느 디스패치 경로를
// 거쳐 오든 `kSyncCr3`/`kSyncRsp0ForDispatch`가 매번 먼저 실행되므로
// 이 함수 자신은 그 동기화를 신경 쓸 필요가 없다). 여기서부터는
// 파킹 위험이 있는 공유 스크래치 스택이 아니라 이 스레드만의 안전한
// 스택이므로, 이후 재시도는 기존 `Scheduler::parkCurrent()`(범용
// `kContextSwitch`)를 그대로 써도 안전하다 - 이게 바로 이 리디자인의
// 핵심(딱 한 번, 공유 스크래치 스택 -> 이 안전한 전용 스택으로
// 건너오는 첫 전환만 프레임 기반으로 하면 충분하고, 그 뒤로는 이미
// 검증된 기존 메커니즘을 그대로 재사용할 수 있다).
extern "C" [[noreturn]] void kSyscallResumeTrampolineBody() {
    asm volatile("sti");
    auto* self = static_cast<kernel::UserThread*>(kernel::Scheduler::currentTask());

    kernel::Syscall::MultiWaitResult result;
    for (;;) {
        auto outcome = kernel::kCheckPendingWaitOnce(self, self->pendingWaitTokens, self->pendingWaitCount);
        if (outcome.found) {
            result = outcome.result;
            break;
        }
        asm volatile("cli");
        // 이제 안전하다 - 이 재개 지점은 이 스레드 전용 스택
        // (kernelStackTop) 위의 이 루프 자신이다(공유 인터럽트
        // 디스패치 스택이 아님).
        kernel::Scheduler::parkCurrent();
    }

    // [PN-22E5E9E7 항목7] idt.cpp의 kSyscallVerbWait/kSyscallVerbWaitAnyOf
    // 분기가 원래 하던 결과 마샬링과 정확히 같은 일을 여기서 대신
    // 한다(그 분기들의 정상 반환 경로는 우리가 파킹한 이 경우엔 다시는
    // 실행되지 않으므로).
    kernel::uint64_t raxValue;
    if (self->pendingWaitAnyOfArgs) {
        self->pendingWaitAnyOfArgs->resultToken = result.token;
        self->pendingWaitAnyOfArgs->resultOutcome = result.outcome;
        raxValue = 1;
        self->pendingWaitAnyOfArgs = nullptr;
    } else {
        raxValue = (result.outcome == kernel::Syscall::MultiWaitOutcome::Completed) ? 1 : 0;
    }

    // [PN-22E5E9E7 항목7] `kDispatchSyscallVerb`(idt.cpp)의 정상 반환
    // 경로가 항상 하는 FS_BASE 유저값 복원 - 우리는 그 wrapper를
    // 우회해 곧장 ring3로 돌아가므로 반드시 직접 호출해야 한다(안
    // 하면 유저 스레드가 커널 FS_BASE로 계속 실행되는 조용한 버그가
    // 된다 - TLS/스레드-로컬 접근이 전부 깨짐).
    kernel::kSyncFsBaseToUser(self);

    self->pendingSyscallReturnFrame.rax = raxValue;
    kernel::kJumpToFrame(&self->pendingSyscallReturnFrame);
    __builtin_unreachable();
}
