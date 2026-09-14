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

// 이 코어에서 지금 실행/재개 중인 AsyncTask - reactorTaskEntry만
// 갱신한다. nullptr이면 리액터가 popFront()/parkCurrent() 사이 어딘가.
kernel::AsyncTask* gCurrentAsyncTask[kMaxCores] = {};

// 리액터 Task가 AsyncTask로 전환하기 직전의 자기 자신(리액터
// 컨텍스트) RSP - AsyncTask::yield()가 돌아올 자리.
kernel::uint64_t gReactorSavedRsp[kMaxCores] = {};

// 코어당 전용 리액터 Task 실체 - 일반 kernel::Task 그대로, Scheduler가
// 다른 Task와 동일하게 다룬다(다만 즉시 스케줄링으로만 깨어남).
kernel::Task gReactorTasks[kMaxCores];

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
        return;  // 리액터 컨텍스트에서(즉 AsyncTask 바깥에서) 잘못 호출된 경우
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
        AsyncTask* task = gExecQueues[coreIndex].popFront();
        if (!task) {
            Scheduler::parkCurrent();
            continue;  // 깨어나면(submitCompletion) 다시 popFront부터
        }
        if (task->state == AsyncTaskState::Ready) {
            task->state = AsyncTaskState::Running;
        } else if (task->state == AsyncTaskState::Suspended) {
            task->state = AsyncTaskState::Running;
        }
        gCurrentAsyncTask[coreIndex] = task;
        kContextSwitch(&gReactorSavedRsp[coreIndex], task->savedRsp);
        gCurrentAsyncTask[coreIndex] = nullptr;

        if (task->state == AsyncTaskState::Completed || task->state == AsyncTaskState::Failed) {
            // 이 AsyncTask가 끝나기를 기다리는 kernel::Task가 있으면
            // (예: Syscall::wait) 먼저 깨운다 - 이 코어에서 실행됐으니
            // 대기자도 반드시 같은 코어에서 파킹돼 있다(v1 - 코어 간
            // 이관 없음). 아래에서 task를 반납하기 전에 반드시 먼저
            // 읽어야 한다(반납 후엔 이 필드도 더 이상 유효하지 않음).
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

void AsyncReactor::submitCompletion(AsyncTask* task) {
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    gExecQueues[coreIndex].pushBack(task);
    // 리액터가 이미 실행 중이면(다른 AsyncTask를 처리 중이거나 막
    // popFront하러 가는 길이면) 다음 자기 루프에서 자연히 이 항목을
    // 집어간다 - 다시 깨울 필요가 없을 뿐더러, 파킹돼 있지 않은
    // Task를 scheduleImmediate로 또 큐에 넣으면 이중 스케줄링이 된다
    // (runLoop/yieldCurrent에서 실측으로 발견한 것과 같은 종류의 버그).
    if (Scheduler::currentTask() != &gReactorTasks[coreIndex]) {
        Scheduler::scheduleImmediate(coreIndex, &gReactorTasks[coreIndex]);
    }
}

}  // namespace kernel
