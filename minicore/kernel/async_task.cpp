#include "async_task.h"

#include "acpi.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"
#include "scheduler.h"
#include "task.h"

namespace {

extern "C" void kTaskStartTrampoline();

// AsyncTask 전용 진입 래퍼 - kTaskStartTrampoline(context_switch.S)이
// "call rbx"로 이 함수를 호출한다(rdi = r12 = AsyncTask* 그대로). Task와
// 달리 이 함수가 "반환"하면 안 된다(반환하면 트램폴린의 halt 루프로
// 떨어져 이 코어가 영원히 멈춘다) - 그래서 onExec 실행 뒤 반드시
// AsyncTask::yield()류의 kContextSwitch로 리액터에 영구 복귀한다.
void kAsyncTaskEntryWrapper(void* arg) {
    auto* task = static_cast<kernel::AsyncTask*>(arg);
    kernel::AsyncTaskHandler* handler = kernel::AsyncCallbackRegistry::resolve(task->subjectCode);
    if (handler) {
        handler->onExec(task, task->args);
        task->state = kernel::AsyncTaskState::Completed;
    } else {
        // 등록되지 않은 subjectCode로 제출된 경우 - 설계/구현 오류지만
        // 이 코어를 멈추지 않기 위해 실패로만 표시하고 계속 진행한다.
        task->state = kernel::AsyncTaskState::Failed;
    }
    kernel::AsyncTask::yield();
    // yield()가 이 AsyncTask를 다시 스케줄하지 않으므로(리액터가 상태를
    // 보고 정리) 이 지점으로 다시는 돌아오지 않는다 - 방어적 무한 루프.
    for (;;) {
    }
}

constexpr kernel::uint32_t kMaxCores = kernel::kAcpiMaxCpus;

// AsyncTask::next 침습적 포인터를 재사용하는 단일 연결 리스트 -
// scheduler.h의 TaskQueue와 정확히 같은 패턴(Spinlock 폴백).
class AsyncTaskQueue {
public:
    void pushBack(kernel::AsyncTask* task) {
        kernel::SpinlockGuard guard(_lock);
        task->next.store(nullptr);
        if (_tail) {
            _tail->next.store(task);
        } else {
            _head = task;
        }
        _tail = task;
    }

    kernel::AsyncTask* popFront() {
        kernel::SpinlockGuard guard(_lock);
        kernel::AsyncTask* task = _head;
        if (task) {
            _head = task->next.load();
            if (!_head) {
                _tail = nullptr;
            }
            task->next.store(nullptr);
        }
        return task;
    }

private:
    kernel::Spinlock _lock;
    kernel::AsyncTask* _head = nullptr;
    kernel::AsyncTask* _tail = nullptr;
};

AsyncTaskQueue gExecQueues[kMaxCores];

// 선점 큐(SP-00CA7175 §2.2, PN-7AC01E6E 항목 6) - exclusivePreemptive
// Channel의 connectChannel/acceptFromChannel 핸드셰이크 완료에서만
// 채워진다(async_task.h의 submitCompletion 주석 참고). reactorTaskEntry가
// 매 루프마다 이 큐를 gExecQueues보다 먼저 비운다 - 그 외에는 완전히
// 동일한 큐 구현을 재사용.
AsyncTaskQueue gPreemptiveQueues[kMaxCores];

// 이 코어에서 지금 실행/재개 중인 AsyncTask - reactorTaskEntry만
// 갱신한다. nullptr이면 리액터가 popFront()/parkCurrent() 사이 어딘가.
kernel::AsyncTask* gCurrentAsyncTask[kMaxCores] = {};

// 리액터 Task가 AsyncTask로 전환하기 직전의 자기 자신(리액터
// 컨텍스트) RSP - AsyncTask::yield()가 돌아올 자리.
kernel::uint64_t gReactorSavedRsp[kMaxCores] = {};

// 코어당 전용 리액터 Task 실체 - 일반 kernel::Task 그대로, Scheduler가
// 다른 Task와 동일하게 다룬다(다만 즉시 스케줄링으로만 깨어남).
kernel::Task gReactorTasks[kMaxCores];

// 이 리액터가 지금 "명시적으로 깨워 줘야만 다시 도는" 상태(진짜
// Scheduler::parkCurrent()로 블로킹됨)인지 - 실측으로 발견한 경쟁
// (2026-09-14, Channel IPC 스트레스 테스트): 원래
// AsyncReactor::submitCompletion()은 Scheduler::currentTask() !=
// &gReactorTasks[coreIndex]로 "리액터가 지금 안 돌고 있으니 깨워야
// 한다"를 판단했는데, 이 신호는 스케줄러 틱이 리액터 Task를(어느
// AsyncTask의 onExec를 대신 실행하는 도중이든, popFront 직후 막
// parkCurrent()를 부르려던 참이든) Task 수준에서 그냥 보통의
// 라운드로빈으로 선점해 버리면 완전히 어긋난다 - 그 순간
// gCurrentTask[coreIndex]는 더 이상 리액터가 아니게 되지만, 리액터
// Task 자신은 (parkCurrent()를 실제로 부른 게 아니라 그냥 Ready로
// 재큐잉될 뿐이므로) 이미 스스로 다시 스케줄될 수 있는 상태다 -
// 그런데도 currentTask() 기반 판단은 "안 돌고 있다"고 오판해
// scheduleImmediate로 또 다른 큐에 넣어 버려, 같은 Task가 두 큐에
// 동시에 들어가는 이중 스케줄링이 된다(runLoop/yieldCurrent에서 이미
// 실측 발견한 것과 같은 근본 원인). 이 플래그는 "진짜로 명시적 wake가
// 필요한가"만을 오직 reactorTaskEntry() 자신이(parkCurrent() 호출
// 직전, cli로 보호된 구간에서) true로 세우고, submitCompletion()이
// 그 값을 확인+false로 되돌리는 것으로 대체해 이 오판을 근본적으로
// 없앤다 - currentTask()가 무엇이든(틱 선점으로 바뀌었든 말든) 상관
// 없이 항상 정확하다.
bool gReactorParked[kMaxCores] = {};

constexpr kernel::uint32_t kMaxHandlers = 64;  // v1 상한 - 필요해지면 늘림
kernel::AsyncTaskHandler* gHandlers[kMaxHandlers] = {};
kernel::uint32_t gNextSubjectCode = 0;
kernel::Spinlock gRegistryLock;

}  // namespace

namespace kernel {

void AsyncTask::init(AsyncTaskSubjectCode subjectCodeIn, AsyncTaskManageCode manageCodeIn, void* argsIn) {
    subjectCode = subjectCodeIn;
    manageCode = manageCodeIn;
    args = argsIn;
    state = AsyncTaskState::Ready;
    next.store(nullptr);
    // AsyncTask는 항상 raw slab 메모리 위에 reinterpret_cast로 앉혀지고
    // (placement new 없음) - 기본 멤버 초기화식은 실행되지 않으므로
    // 여기서 전부 명시적으로 리셋해야 한다. 특히 waitingTask를 안
    // 지우면 슬랩 재사용으로 이전 점유자의 낡은 포인터가 남아, 리액터가
    // 완료 시 엉뚱한(이미 해제됐을 수도 있는) Task를 깨우려 든다.
    waitingTask = nullptr;
    ownerTask = nullptr;
    autoFree = true;

    void* stack = GenericSlabAllocator::alloc(kAsyncTaskStackSize);
    if (!stack) {
        stackBase = 0;
        savedRsp = 0;
        return;  // 호출부(submit)가 stackBase==0을 확인해 실패 처리해야 한다
    }
    stackBase = reinterpret_cast<uint64_t>(stack);

    // kContextSwitch의 pop 순서(r15,r14,r13,r12,rbx,rbp,popfq,ret)와
    // 정확히 대응하도록 스택을 구성한다 - task.cpp의 Task::init()과
    // 완전히 동일한 레이아웃, entry만 kAsyncTaskEntryWrapper로 고정하고
    // arg는 이 AsyncTask 자신(this)이다.
    const uint64_t stackTop = stackBase + kAsyncTaskStackSize;
    auto* sp = reinterpret_cast<uint64_t*>(stackTop);
    *(--sp) = reinterpret_cast<uint64_t>(&kTaskStartTrampoline);
    *(--sp) = 0x202;                                            // RFLAGS: IF=1
    *(--sp) = 0;                                                // rbp
    *(--sp) = reinterpret_cast<uint64_t>(&kAsyncTaskEntryWrapper);  // rbx -> 트램폴린이 call
    *(--sp) = reinterpret_cast<uint64_t>(this);                 // r12 -> 트램폴린이 rdi로 옮김
    *(--sp) = 0;                                                // r13
    *(--sp) = 0;                                                // r14
    *(--sp) = 0;                                                // r15

    savedRsp = reinterpret_cast<uint64_t>(sp);
}

void AsyncTask::yield() {
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    AsyncTask* self = gCurrentAsyncTask[coreIndex];
    if (!self) {
        return;  // 리액터 컨텍스트에서(즉 AsyncTask 밖에서) 잘못 호출된 경우
    }
    if (self->state == AsyncTaskState::Running) {
        self->state = AsyncTaskState::Suspended;
    }
    // AsyncTask는 스케줄러 틱 기반 선점 대상이 아니다(리액터가 이
    // AsyncTask를 실행 중인 동안, 바깥의 kernel::Task 수준에서는
    // 여전히 "리액터 Task가 실행 중"이므로 그 틱 선점은 정상적으로
    // 별개로 계속 작동한다 - kContextSwitch가 RSP만 넘나드는 값이라
    // 어느 스택 위에서 틱이 발생하든 저장/복원이 그대로 성립한다).
    // 그래서 여기서는 cli 없이 그냥 전환해도 안전하다 - onTick/yieldCurrent
    // 쪽의 "같은 Task가 큐와 currentTask에 동시에 존재" 경쟁과 달리,
    // submitCompletion은 이 AsyncTask 자체를 currentTask 여부로 분기하지
    // 않고 그냥 큐에 넣기만 하므로 이중 스케줄링 경로가 없다.
    kContextSwitch(&self->savedRsp, gReactorSavedRsp[coreIndex]);
    // 리액터가 이 AsyncTask를 다시 뽑아 재개하면 이 지점으로 돌아온다.
}

AsyncTask* AsyncTask::submit(AsyncTaskSubjectCode subjectCode, AsyncTaskManageCode manageCode, void* args,
                              bool autoFree) {
    void* mem = GenericSlabAllocator::alloc(sizeof(AsyncTask));
    if (!mem) {
        return nullptr;
    }
    auto* task = reinterpret_cast<AsyncTask*>(mem);
    task->init(subjectCode, manageCode, args);
    if (!task->stackBase) {
        GenericSlabAllocator::free(mem, sizeof(AsyncTask));
        return nullptr;
    }
    // autoFree는 init()이 리셋한 뒤, 리액터에 보여 완료될 수 있게 되기
    // 전에(submitCompletion 호출 전에) 반드시 설정해야 한다 - 그렇지
    // 않으면 아주 빨리 완료되는 작업이 기본값(true)으로 자동 반납될
    // 수 있다(경쟁).
    task->autoFree = autoFree;
    // [PN-40E976F2] 이 AsyncTask의 실제 소유자는 지금 이 호출을 하고
    // 있는 코어의 리액터다 - submit()이 항상 그 코어에서 곧바로
    // submitCompletion()을 부르므로(아래) 코어가 갈릴 일이 없다.
    task->ownerTask = &gReactorTasks[Scheduler::currentCoreIndex()];
    AsyncReactor::submitCompletion(task);
    return task;
}

AsyncTaskSubjectCode AsyncCallbackRegistry::registerHandler(AsyncTaskHandler* handler) {
    SpinlockGuard guard(gRegistryLock);
    if (gNextSubjectCode >= kMaxHandlers) {
        return kMaxHandlers;  // 상한 초과 - 호출부가 resolve()로 재확인하면 항상 nullptr
    }
    const AsyncTaskSubjectCode code = gNextSubjectCode++;
    gHandlers[code] = handler;
    return code;
}

AsyncTaskHandler* AsyncCallbackRegistry::resolve(AsyncTaskSubjectCode subjectCode) {
    if (subjectCode >= kMaxHandlers) {
        return nullptr;
    }
    return gHandlers[subjectCode];
}

void AsyncReactor::initForThisCore() {
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    gReactorTasks[coreIndex].init(reactorTaskEntry, nullptr);
    // 최초 1회는 일반 큐에 넣어 실행되게 한다 - 실행되자마자 할 일이
    // 없으면 곧장 parkCurrent()로 잠든다.
    Scheduler::enqueue(coreIndex, &gReactorTasks[coreIndex]);
}

void AsyncReactor::reactorTaskEntry(void*) {
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    for (;;) {
        // 선점 큐(§2.2)를 항상 먼저 확인한다 - 비어 있으면 일반 큐로.
        AsyncTask* task = gPreemptiveQueues[coreIndex].popFront();
        if (!task) {
            task = gExecQueues[coreIndex].popFront();
        }
        if (!task) {
            // gReactorParked를 "진짜로 블로킹되는" 이 순간에만 true로
            // 세운다 - cli로 이 대입과 parkCurrent()의 실제 전환 사이를
            // 하나로 묶어, 그 틈에 스케줄러 틱이 끼어들어도(리액터를
            // 그냥 보통의 라운드로빈으로 재큐잉해 버리는 경우도 포함)
            // submitCompletion()이 이 플래그만 보고 정확히 판단할 수 있게
            // 한다(위 gReactorParked 선언부 주석 참고 - cli는
            // parkCurrent() 안의 cli와 중복이라 무해하다).
            asm volatile("cli");
            gReactorParked[coreIndex] = true;
            Scheduler::parkCurrent();
            continue;  // 깨어나면(submitCompletion) 다시 popFront부터
        }
        if (task->state == AsyncTaskState::Cancelled) {
            // [PN-40E976F2] 이 AsyncTask를 기다리던 UserThread가 이미
            // 죽어(scheduler.cpp의 SelfTerminateHandler::onExec) 결과를
            // 가져갈 사람이 없다 - onExec을 실행/재개하지 않고 곧장
            // onCancel만 부른 뒤 자원을 반납한다. autoFree는 취소
            // 시점에 이미 강제로 true가 돼 있다(그 시점 이후로는 아무도
            // wait()로 직접 반납할 수 없으므로).
            AsyncTaskHandler* handler = AsyncCallbackRegistry::resolve(task->subjectCode);
            if (handler) {
                handler->onCancel(task, task->args);
            }
            if (task->autoFree) {
                GenericSlabAllocator::free(reinterpret_cast<void*>(task->stackBase), kAsyncTaskStackSize);
                GenericSlabAllocator::free(task, sizeof(AsyncTask));
            }
            continue;
        }
        if (task->state == AsyncTaskState::Ready) {
            task->state = AsyncTaskState::Running;
        } else if (task->state == AsyncTaskState::Suspended) {
            task->state = AsyncTaskState::Running;
        }
        gCurrentAsyncTask[coreIndex] = task;
        {
            // AsyncTask 프레임워크의 원래 설계 의도(kAsyncTaskEntryWrapper
            // 주석 참고 - "리액터가 AsyncTask를 실행하는 동안 바깥
            // kernel::Task 수준에서는 여전히 리액터가 실행 중이어야
            // 한다")를 실제로 강제한다 - 이 구간(AsyncTask가 리액터의
            // 실행 슬롯을 "빌려 쓰는" 동안) 전체를 Task 수준 선점
            // 대상에서 제외한다(Slab 매거진 보호에 쓰는 것과 같은
            // PreemptionGuard 재사용 - 인터럽트 자체는 막지 않아 EOI/
            // 하드웨어 처리는 정상 진행됨). gReactorParked 플래그가
            // 이중 스케줄링 자체는 이미 막아 주지만, 이 가드가 없으면
            // 여전히 리액터가 Task 수준에서 불필요하게 선점->재큐잉될
            // 수 있어 방어적으로 같이 둔다.
            PreemptionGuard guard;
            kContextSwitch(&gReactorSavedRsp[coreIndex], task->savedRsp);
        }
        gCurrentAsyncTask[coreIndex] = nullptr;

        if (task->state == AsyncTaskState::Completed || task->state == AsyncTaskState::Failed) {
            // 이 AsyncTask가 끝나기를 기다리는 kernel::Task가 있으면
            // (예: Syscall::wait) 먼저 깨운다 - 이 코어에서 실행됐으니
            // 대기자도 반드시 같은 코어에서 파킹돼 있다(v1 - 코어 간
            // 이관 없음). 아래에서 task를 반납하기 전에 반드시 먼저
            // 읽어야 한다(반납 후에는 이 필드도 더 이상 유효하지 않음).
            if (task->waitingTask) {
                Scheduler::scheduleImmediate(coreIndex, task->waitingTask);
            }
            // args의 생성/반납은 처리기 책임(SP-F682B889 §3.1) - 여기서는
            // 프레임워크 소유물(AsyncTask 구조체 자신과 그 전용 스택)만,
            // 그것도 autoFree인 경우에만 반납한다 - false면 결과를 아직
            // 못 읽은 소비자(위에서 막 깨운 그 Task)가 직접 반납할
            // 책임을 진다.
            if (task->autoFree) {
                GenericSlabAllocator::free(reinterpret_cast<void*>(task->stackBase), kAsyncTaskStackSize);
                GenericSlabAllocator::free(task, sizeof(AsyncTask));
            }
        }
        // Suspended면 아무 것도 안 함 - 나중에 submitCompletion으로 다시
        // 큐에 들어와야 재개된다.
    }
}

void AsyncReactor::submitCompletion(AsyncTask* task, bool preemptive) {
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    if (preemptive) {
        gPreemptiveQueues[coreIndex].pushBack(task);
    } else {
        gExecQueues[coreIndex].pushBack(task);
    }
    // 실측으로 발견한 경쟁(2026-09-14, Channel IPC 스트레스 테스트):
    // 원래 여기서는 Scheduler::currentTask() != &gReactorTasks[coreIndex]
    // 로 "리액터가 지금 안 돌고 있다"를 판단했는데, 스케줄러 틱이
    // 리액터 Task를(어느 onExec 실행 도중이든, popFront 직후 막
    // parkCurrent()를 부르려던 참이든) Task 수준에서 그냥 보통의
    // 라운드로빈으로 선점해 버리면 이 판단이 완전히 어긋난다 - 그
    // 순간 currentTask()는 더 이상 리액터가 아니지만, 리액터 자신은
    // (parkCurrent()를 실제로 부른 게 아니므로) 이미 스스로 다시
    // 스케줄될 수 있는 상태다. 그런데도 "안 돌고 있다"고 오판해
    // scheduleImmediate로 또 다른 큐에 넣으면, 같은 Task가 두 큐에
    // 동시에 들어가는 이중 스케줄링이 된다(runLoop/yieldCurrent에서
    // 이미 실측 발견한 것과 같은 근본 원인, PL-2D3184BC 참고). 대신
    // gReactorParked(reactorTaskEntry가 parkCurrent() 호출 직전
    // cli로 보호된 구간에서만 true로 세우는 전용 플래그)를 확인+
    // 소비한다 - currentTask()가 무엇이든(틱 선점으로 바뀌었든 말든)
    // 상관없이 "진짜로 명시적 wake가 필요한가"만 정확히 반영한다.
    // 이 함수는 인터럽트 컨텍스트에서도 호출 가능하다고 문서화돼
    // 있어(async_task.h) 무조건 sti로 끝내면 안 된다 - 원래 RFLAGS.IF
    // 값을 저장해 뒀다가 그 값이었을 때만 되돌린다(호출 전 인터럽트가
    // 꺼져 있던 컨텍스트라면 계속 꺼진 채로 반환).
    uint64_t rflags;
    asm volatile("pushfq; pop %0; cli" : "=r"(rflags));
    const bool wasParked = gReactorParked[coreIndex];
    gReactorParked[coreIndex] = false;
    if (rflags & (1ULL << 9)) {
        asm volatile("sti");
    }
    if (wasParked) {
        Scheduler::scheduleImmediate(coreIndex, &gReactorTasks[coreIndex]);
    }
}

}  // namespace kernel
