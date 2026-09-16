#include "async_task.h"

#include "acpi.h"
#include "delayed_exec.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "lapic.h"
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

// 이 코어에서 "지금 이 자리"(runLoop()의 idle 인라인 호출이든, §4 (C)
// 경로의 IPI 핸들러든)가 AsyncTask로 전환하기 직전의 자기 자신 RSP -
// AsyncTask::yield()가 돌아올 자리(옛 "리액터 Task 컨텍스트"와 정확히
// 같은 역할, 이름만 유지 - 별도 Task가 아니게 됐다고 해서 이 슬롯
// 자체의 의미가 바뀌지는 않는다).
kernel::uint64_t gReactorSavedRsp[kMaxCores] = {};

// AsyncReactor::drainOnce()의 재진입 방지 플래그(2026-09-16 재구조,
// PN-FEAAF154) - 이미 이 코어에서 드레인이 진행 중일 때(gReactorSavedRsp
// 슬롯이 사용 중일 때) §4 (C) 경로의 IPI가 겹쳐 들어와도 또 다른
// drainOnce() 호출이 같은 슬롯을 건드리지 않도록 막는다. 옛
// `gReactorParked`(리액터 Task의 park/wake 상태 추적)와는 목적이
// 다르다 - Task 자체가 없어졌으므로 그 개념은 폐기됐다.
bool gDraining[kMaxCores] = {};

// §4 (C) 경로("다른 Task 실행 중일 때"의 즉시 개입, QU-3BDEE348 답변)
// 전용 IPI 벡터 - RM-28225668에 배정(0xE3, kForcedMigrationVector
// (SP-ECC59BAE, 0xE2) 다음 번호). ISR은 이 코어 자신에게만 보내는
// self-IPI를 받는다 - x86 표준 동작대로 인터럽트가 다시 켜지는 순간
// (대개 submitCompletion() 호출부가 반환하며 sti하는 시점, 또는 이미
// sti 상태였다면 그 즉시) 전달된다.
constexpr kernel::uint32_t kAsyncDrainVector = 0xE3;

void kAsyncDrainIsr(kernel::InterruptFrame*) {
    const kernel::uint32_t coreIndex = kernel::Scheduler::currentCoreIndex();
    // 이 IPI가 도착한 시점에 큐에 있던 것 전부를 이 자리에서 처리한다 -
    // 그 사이 또 들어온 게 있으면(드문 경쟁) 다음 IPI가 마저 처리하므로
    // 무한정 여기 머무르지 않는다. EOI는 공통 ISR 스텁이 처리
    // (tlb_shootdown.cpp와 동일 관례).
    while (kernel::AsyncReactor::drainOnce(coreIndex)) {
    }
}

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
    AsyncReactor::submitCompletion(task);
    return task;
}

namespace {

bool kIsAsyncTaskTerminal(AsyncTaskState state) {
    return state == AsyncTaskState::Completed || state == AsyncTaskState::Failed ||
           state == AsyncTaskState::Cancelled;
}

}  // namespace

void AsyncTaskGroup::add(AsyncTask* task) {
    _tasks.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    _tasks.insert(task);
}

uint32_t AsyncTaskGroup::pendingCount() {
    uint32_t pending = 0;
    // forEach 도중 erase()는 안전하다 - 그 슬롯의 used를 false로
    // 내릴 뿐 순회 중인 청크/인덱스 구조 자체는 바뀌지 않는다.
    _tasks.forEach([&](AsyncTask*& task, ChunkedList<AsyncTask*, kChunkCapacity>::Slot* slot) {
        if (kIsAsyncTaskTerminal(task->state)) {
            GenericSlabAllocator::free(reinterpret_cast<void*>(task->stackBase), kAsyncTaskStackSize);
            GenericSlabAllocator::free(task, sizeof(AsyncTask));
            _tasks.erase(slot);
        } else {
            ++pending;
        }
    });
    return pending;
}

void AsyncTaskWaitGroup::waitAll() {
    // **실측으로 발견한 버그(2026-09-16) 수정**: Scheduler::yieldCurrent()
    // 만으로는 리액터가 절대 실행될 기회를 못 얻는다 - runLoop()은
    // pickNext()가 뭔가를 찾는 한(이 호출 자신이 yieldCurrent()로 막
    // 다시 큐에 넣은 그 Task 자신을 즉시 재선택하는 경우 포함) 리액터
    // 드레인(idle 분기)으로 절대 안 넘어간다 - 이 Task가 유일한 Ready
    // Task이면 yieldCurrent()가 자기 자신에게 즉시 되돌아오는 무한
    // 루프가 된다(다른 Ready Task가 있을 때만 우연히 그 사이 idle이
    // 낄 여지가 생긴다 - 신뢰할 수 없는 전제). §4 (C) 경로가 이미
    // 증명한 대로 drainOnce()는 idle 컨텍스트 전용이 아니라 어떤
    // 실행 흐름에서 불러도 안전하므로, 직접 능동적으로 드레인을
    // 시도하고 - 정말 할 일이 없을 때만(false) yieldCurrent()로 다른
    // Task에게 양보한다.
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    while (_group.pendingCount() > 0) {
        if (!AsyncReactor::drainOnce(coreIndex)) {
            Scheduler::yieldCurrent();
        }
    }
}

void AsyncTaskAwaiter::await() {
    // **실측으로 발견한 버그(2026-09-16) 수정**: AsyncTask::yield()는
    // 그저 리액터 쪽으로 제어를 돌려줄 뿐, 그 뒤 아무도 다시 이
    // AsyncTask를 실행 큐에 넣어 주지 않는 한(예: Channel 핸드셰이크
    // 처럼 상대편이 명시적으로 submitCompletion() 해 주는 협조적
    // 관계) 영원히 Suspended 상태로 멈춰 있는다 - onCancel/유저
    // 소유자 종료와 무관한 "임의의 관계없는 AsyncTask 하나를 그냥
    // 기다리는" 이 범용 Awaiter에는 그런 협조자가 없어, §3.3 원안
    // 그대로("yield 반복") 구현하면 첫 yield() 이후 영원히 멈춘다
    // (실측 확인). 그래서 매번 양보하기 직전 스스로를 다시 제출해
    // (drainOnce()가 이미 지원하는 "Suspended면 Running으로 되돌려
    // 재개" 경로를 그대로 재사용) 다음 드레인 차례에 반드시 다시
    // 뽑히도록 만든다 - AsyncTask 구조체에 새 필드를 추가하지 않고도
    // (QU-86DD998F 답변 그대로) 이 AsyncTask 자신의 기존 재개
    // 메커니즘만으로 해결된다.
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    while (!kIsAsyncTaskTerminal(_target->state)) {
        AsyncTask* self = gCurrentAsyncTask[coreIndex];
        if (self) {
            AsyncReactor::submitCompletion(self);
        }
        AsyncTask::yield();
    }
    GenericSlabAllocator::free(reinterpret_cast<void*>(_target->stackBase), kAsyncTaskStackSize);
    GenericSlabAllocator::free(_target, sizeof(AsyncTask));
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

void AsyncReactor::init() {
    // 전역 IDT 등록이라 BSP에서 한 번만(tlb_shootdown.cpp와 동일한
    // 이유) - AP는 이 클래스를 위해 더 이상 아무것도 부를 필요가 없다
    // (코어별 큐는 이미 정적 배열, 전용 Task 자체가 없어졌다).
    Idt::registerHandler(kAsyncDrainVector, kAsyncDrainIsr);
}

bool AsyncReactor::drainOnce(uint32_t coreIndex) {
    if (gDraining[coreIndex]) {
        // 이미 이 코어에서(runLoop() 인라인 호출이든 §4 (C) IPI
        // 핸들러든) 드레인이 진행 중 - gReactorSavedRsp[coreIndex]를
        // 두 번 건드리면 진행 중인 AsyncTask의 재개 지점이 깨진다.
        // 새로 큐잉된 항목은 바깥쪽 호출이 이어서 처리하므로 유실
        // 걱정 없다.
        return false;
    }

    // 선점 큐(§2.2)를 항상 먼저 확인한다 - 비어 있으면 일반 큐로.
    AsyncTask* task = gPreemptiveQueues[coreIndex].popFront();
    if (!task) {
        task = gExecQueues[coreIndex].popFront();
    }
    if (!task) {
        // 지연 실행 큐(SP-F15B4A63, PN-C46DF296) - 전용 커널 Task를
        // 새로 만들지 않고 이 코어의 idle 분기에서 만료 타이머를
        // 처리한다(§3, QU-A8C0CC2C 설계자 답변). pump()가 만료된
        // 콜백을 실행하는 도중 AsyncTask::submit()으로 새 작업을 큐에
        // 넣을 수 있으므로, 포기하기 전에 먼저 실행 큐를 한 번 더
        // 확인한다.
        DelayedExecutionQueue::pump();
        task = gPreemptiveQueues[coreIndex].popFront();
        if (!task) {
            task = gExecQueues[coreIndex].popFront();
        }
    }
    if (!task) {
        // 정말 아무 것도 없다 - 호출부(runLoop()의 idle 분기)가 그대로
        // hlt로 진행한다. 아직 만료 안 된 타이머가 남아 있어도 별도
        // 조치가 필요 없다(2026-09-16 재구조로 해소) - 스케줄러 틱이
        // 이미 100Hz로 모든 idle 코어를 hlt에서 깨우므로, 다음 틱에서
        // 이 함수가 다시 호출되면 그때 다시 확인된다.
        return false;
    }

    gDraining[coreIndex] = true;

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
        gDraining[coreIndex] = false;
        return true;
    }

    if (task->state == AsyncTaskState::Ready || task->state == AsyncTaskState::Suspended) {
        task->state = AsyncTaskState::Running;
    }
    gCurrentAsyncTask[coreIndex] = task;
    {
        // AsyncTask 프레임워크의 원래 설계 의도(kAsyncTaskEntryWrapper
        // 주석 참고 - "AsyncTask를 실행하는 동안 바깥 kernel::Task
        // 수준에서는 여전히 (예전 리액터에 해당하는) 그 흐름이 실행
        // 중이어야 한다")를 실제로 강제한다 - 이 구간(AsyncTask가 이
        // 실행 슬롯을 "빌려 쓰는" 동안) 전체를 Task 수준 선점 대상에서
        // 제외한다(Slab 매거진 보호에 쓰는 것과 같은 PreemptionGuard
        // 재사용 - 인터럽트 자체는 막지 않아 EOI/하드웨어 처리는 정상
        // 진행됨).
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
    gDraining[coreIndex] = false;
    return true;
}

void AsyncReactor::submitCompletion(AsyncTask* task, bool preemptive) {
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    if (preemptive) {
        gPreemptiveQueues[coreIndex].pushBack(task);
    } else {
        gExecQueues[coreIndex].pushBack(task);
    }
    // [재설계, 2026-09-16, QU-3BDEE348 답변 - 하이브리드 (A)+(C)]
    // 리액터가 더 이상 Task가 아니므로(async_task.h 주석 참고) "파킹돼
    // 있으면 강제 스케줄링" 판단 자체가 사라졌다 - preemptive==false
    // (A)는 조치 없이 그냥 반환(다음 idle 분기에서 자연히 처리),
    // preemptive==true(C)만 이 코어 자신에게 kAsyncDrainVector IPI를
    // 보내 인터럽트가 다시 켜지는 즉시 drainOnce()가 반복 호출되게
    // 한다. 이 함수는 인터럽트 컨텍스트에서도 호출 가능하다고
    // 문서화돼 있어(async_task.h) self-IPI 발사 자체는 안전하다 -
    // Lapic::sendFixedIpi()는 그저 ICR에 쓰는 것뿐이라 재진입 문제가
    // 없다.
    if (preemptive) {
        Lapic::sendFixedIpi(Acpi::cpuApicId(coreIndex), static_cast<uint8_t>(kAsyncDrainVector));
    }
}

}  // namespace kernel
