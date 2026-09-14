#include "scheduler.h"

#include "acpi.h"
#include "interrupt_frame.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "page_frame_allocator.h"
#include "syscall.h"
#include "timer.h"

namespace kernel {

void TaskQueue::pushBack(Task* task) {
    SpinlockGuard guard(_lock);
    task->next.store(nullptr);
    if (_tail) {
        _tail->next.store(task);
    } else {
        _head = task;
    }
    _tail = task;
}

void TaskQueue::pushFront(Task* task) {
    SpinlockGuard guard(_lock);
    task->next.store(_head);
    _head = task;
    if (!_tail) {
        _tail = task;
    }
}

Task* TaskQueue::popFront() {
    SpinlockGuard guard(_lock);
    Task* task = _head;
    if (task) {
        _head = task->next.load();
        if (!_head) {
            _tail = nullptr;
        }
        task->next.store(nullptr);
    }
    return task;
}

bool TaskQueue::isEmpty() const {
    return _head == nullptr;
}

namespace {

constexpr uint32_t kMaxCores = kAcpiMaxCpus;

// 3단 우선순위(6단계 RT 클래스 + 8-1단계 즉시 스케줄링) - pickNext가
// 이 순서(immediate -> rt -> normal)로 훑는다. 셋을 하나로 합치지
// 않은 이유: "즉시 스케줄링은 RT보다도 먼저"를 큐 자체의 우선순위로
// 표현하면 pushFront 같은 순서 트릭 없이 자명해진다.
TaskQueue gImmediateQueues[kMaxCores];
TaskQueue gRtQueues[kMaxCores];
TaskQueue gNormalQueues[kMaxCores];
uint32_t gCoreCount = 1;

// retireCurrentTask()가 넣고 runLoop()이 드레인하는 "종료된 Task"
// 큐(PL-2D3184BC "Task 종료 프로토콜", QU-26F9420E) - TaskQueue를
// 그대로 재사용한다(우선순위 개념이 없는 단순 FIFO면 충분).
TaskQueue gCleanupQueues[kMaxCores];

// HPET가 없는 폴백 환경에서 전역 tickCount 공급원 역할을 대신하는
// BSP 코어 인덱스(DC-0CC88ABB/QU-3218B790 설계자 답변 (a), 2026-09-14
// - "BSP 한정으로 Scheduler::onTick이 Timer::onTick()도 대신 호출").
// startTickOnThisCore()의 첫 호출(항상 BSP 자신 - AP는 그 이후
// Smp::startApCores()가 순차 기동)에서 한 번만 확정한다.
uint32_t gBspCoreIndex = 0;
bool gBspCoreIndexKnown = false;

// task.cpp의 kOrderForStackSize와 동일한 계산 - kernelStackSize(항상
// 4KiB의 배수)를 되돌려 PageFrameAllocator::freeOrder에 넘길 order를
// 구한다. Task::kernelStackSize는 Task::init()이 이미 4096<<order
// 형태로만 채우므로 이 역산은 항상 정확히 떨어진다.
uint32_t kOrderForCleanup(uint64_t stackSize) {
    uint64_t pages = stackSize / 4096UL;
    uint32_t order = 0;
    while ((1UL << order) < pages) {
        ++order;
    }
    return order;
}

// runLoop()이 이 코어에서 마지막으로 Task를 진입시키기 직전의 자기
// 자신(idle 컨텍스트) RSP를 저장해 둔다 - yieldCurrent()가 돌아올
// 자리. onTick()의 Task-to-Task 직접 전환은 이 값을 안 건드린다
// (idle을 거치지 않고 바로 다음 Task로 가므로).
uint64_t gIdleSavedRsp[kMaxCores] = {};

// 이 코어에서 지금 실행 중인 Task - runLoop()/onTick()/yieldCurrent()
// 만 갱신한다. nullptr이면 idle(runLoop이 pickNext/hlt를 돌고 있음).
Task* gCurrentTask[kMaxCores] = {};

// 선점 비활성화 카운터 - 코어별로 그 코어 자신만 읽고 쓴다(인터럽트
// 게이트라 같은 코어 안에서 재진입 없음, 다른 코어는 자기 배열만
// 건드리므로 원자 연산이 필요 없다).
uint32_t gPreemptDisableCount[kMaxCores] = {};

}  // namespace

void Scheduler::init() {
    gCoreCount = Acpi::cpuCount();
    if (gCoreCount == 0) {
        gCoreCount = 1;
    }
    if (gCoreCount > kMaxCores) {
        gCoreCount = kMaxCores;
    }
}

uint32_t Scheduler::currentCoreIndex() {
    const uint32_t apicId = Lapic::id();
    const uint32_t cpuCount = Acpi::cpuCount();
    for (uint32_t i = 0; i < cpuCount; ++i) {
        if (Acpi::cpuApicId(i) == apicId) {
            return i;
        }
    }
    return 0;
}

void Scheduler::enqueue(uint32_t coreIndex, Task* task) {
    if (coreIndex >= gCoreCount) {
        coreIndex = 0;
    }
    task->state = TaskState::Ready;
    if (task->taskClass == TaskClass::RealTime) {
        gRtQueues[coreIndex].pushBack(task);
    } else {
        gNormalQueues[coreIndex].pushBack(task);
    }
}

void Scheduler::scheduleImmediate(uint32_t coreIndex, Task* task) {
    if (coreIndex >= gCoreCount) {
        coreIndex = 0;
    }
    task->state = TaskState::Ready;
    gImmediateQueues[coreIndex].pushBack(task);
}

Task* Scheduler::pickNext(uint32_t coreIndex) {
    if (coreIndex >= gCoreCount) {
        coreIndex = 0;
    }
    Task* task = gImmediateQueues[coreIndex].popFront();
    if (task) {
        return task;
    }
    task = gRtQueues[coreIndex].popFront();
    if (task) {
        return task;
    }
    return gNormalQueues[coreIndex].popFront();
}

void Scheduler::startTickOnThisCore() {
    if (!gBspCoreIndexKnown) {
        // 이 함수의 첫 호출은 항상 BSP 자신(kMain)에서 온다 - AP는
        // 이후 Smp::startApCores()가 순차 기동하므로 그 시점엔 이미
        // 이 분기를 지난 뒤다(smp.cpp가 이미 전제하는 것과 같은 부팅
        // 순서 가정 - 병렬 AP 기동을 도입하면 재검토 필요).
        gBspCoreIndex = currentCoreIndex();
        gBspCoreIndexKnown = true;
    }
    // 물리 LAPIC 타이머는 코어당 하나뿐이다 - HPET가 있으면
    // Timer::init()이 LAPIC을 아예 건드리지 않으므로(timer.cpp)
    // 여기서 그대로 독점할 수 있다. HPET가 없는 폴백 환경에서는
    // Timer::init()도 더 이상 이 하드웨어를 재프로그램하지 않는다
    // (DC-0CC88ABB/QU-3218B790 설계자 답변 (a), 2026-09-14로 확정 -
    // 대신 onTick()이 BSP에서 Timer::onTick()까지 대신 호출한다).
    Lapic::startPeriodicTimer(kSchedulerTickVector, kSchedulerTickHz);
}

void Scheduler::onTick(InterruptFrame*) {
    // 가장 먼저 EOI - 이 아래서 Task 전환이 일어나면 이 함수 호출은
    // 그 Task가 다시 스케줄될 때까지 "반환"하지 않는다(kContextSwitch
    // 가 콜스택 깊숙이 매달린 채로 남는다) - EOI를 미루면 그 사이
    // 이 코어에 다음 스케줄러 틱이 아예 전달되지 않는다.
    Lapic::sendEoi();

    const uint32_t coreIndex = currentCoreIndex();

    // HPET가 없는 폴백 환경(DC-0CC88ABB/QU-3218B790 설계자 답변 (a),
    // 2026-09-14) - 물리 LAPIC 주기 타이머는 코어당 하나뿐이라 Timer가
    // 별도로 자신의 주기 인터럽트를 프로그램하면 이 스케줄러 틱
    // 자체를 덮어써 버린다(실측 전 리뷰로 확인). 그래서 HPET가 없을
    // 땐 BSP 코어의 이 스케줄러 틱이 전역 시각도 대신 공급한다 -
    // "SMP에서 전역 카운터는 BSP의 카운터를 직접 읽어라"(같은 답변
    // 2번)와 일치하도록 다른 코어는 절대 호출하지 않는다. 선점 금지/
    // idle 여부와 무관하게 항상 불러야 하므로 아래 어떤 조기 반환
    // 보다도 먼저다.
    if (coreIndex == gBspCoreIndex && !Timer::usesHpet()) {
        Timer::onTick();
    }

    if (gPreemptDisableCount[coreIndex] > 0) {
        return;  // 선점 금지 구간 - 인터럽트 자체는 처리됐으니 그냥 계속 실행
    }

    Task* current = gCurrentTask[coreIndex];
    if (!current) {
        return;  // idle 상태 - runLoop의 hlt가 이 인터럽트로 깨어나 pickNext를 다시 확인한다
    }

    Task* next = pickNext(coreIndex);
    if (!next) {
        return;  // 대기 중인 다른 Task 없음 - 그대로 계속 실행(타임퀀텀 소진 안 함)
    }

    enqueue(coreIndex, current);  // 라운드로빈 - Ready로 큐 꼬리에 재삽입
    gCurrentTask[coreIndex] = next;
    next->state = TaskState::Running;
    // current의 커널 스택(지금 이 인터럽트 프레임이 쌓여 있는 바로 그
    // 스택) 위에서 호출 중이라, 나중에 current가 다시 선택되면 이
    // 호출 지점 바로 다음부터 재개되어 자연스럽게 kIsrHandler ->
    // isr_common_stub -> iretq로 이어진다(자기 자신의 InterruptFrame
    // 그대로).
    kContextSwitch(&current->savedRsp, next->savedRsp);
}

void Scheduler::runLoop() {
    const uint32_t coreIndex = currentCoreIndex();
    for (;;) {
        // retireCurrentTask()가 넣어 둔, 이미 끝난 Task들의 커널
        // 스택을 회수한다 - 지금 이 idle 컨텍스트는 그 Task들의
        // 스택 위가 아니므로 안전하다(PL-2D3184BC "Task 종료
        // 프로토콜", QU-26F9420E). pickNext보다 먼저 해도 순서 문제
        // 없다 - 이 큐는 스케줄링 대상이 아니라 순수 회수 대기열이다.
        for (;;) {
            Task* zombie = gCleanupQueues[coreIndex].popFront();
            if (!zombie) {
                break;
            }
            PageFrameAllocator::freeOrder(zombie->kernelStackPhys, kOrderForCleanup(zombie->kernelStackSize));
        }

        Task* next = pickNext(coreIndex);
        if (!next) {
            asm volatile("sti; hlt");
            continue;
        }
        // cli - gCurrentTask를 세팅한 시점과 실제로 next의 스택으로
        // 넘어가는 시점(kContextSwitch 내부의 mov rsp,rsi) 사이에 이
        // 코어의 틱이 끼어들면, onTick이 "next가 이미 실행 중"이라고
        // 착각해 아직 idle 스택 위에 있는 이 kContextSwitch 호출을
        // next 자신의 것처럼 다시 가로채 버린다(next->savedRsp가
        // idle 스택의 스냅샷으로 덮어써짐 - 실측으로 발견한 버그).
        // 여기서 끈 인터럽트는 kContextSwitch의 pushfq/popfq를 통해
        // idle 쪽에만 저장되고(나중에 idle이 재개될 때만 다시 반영),
        // next는 자신이 마지막으로 저장해 둔 RFLAGS(보통 IF=1)로
        // 독립적으로 재개되므로 next 쪽으로 "인터럽트 꺼짐"이 새어
        // 나가지 않는다.
        asm volatile("cli");
        gCurrentTask[coreIndex] = next;
        next->state = TaskState::Running;
        kContextSwitch(&gIdleSavedRsp[coreIndex], next->savedRsp);
        // yieldCurrent()로 되돌아온 경우에만 이 지점으로 온다(onTick의
        // Task-to-Task 전환은 이 프레임을 거치지 않는다) - 다음
        // 루프에서 pickNext가 새 상태를 다시 판단한다. 이 시점의
        // 인터럽트 상태는 idle이 마지막으로 저장했던 그대로(위 cli로
        // 꺼져 있음)이므로, 아래에서 다시 준비 없이 바로 다음
        // pickNext/전환으로 넘어가도 안전하다 - sti는 "정말 대기할
        // 때"(위 hlt 분기)에만 한다.
        gCurrentTask[coreIndex] = nullptr;
    }
}

Task* Scheduler::currentTask() {
    return gCurrentTask[currentCoreIndex()];
}

void Scheduler::yieldCurrent() {
    // Task 실행 흐름은 보통 IF=1(인터럽트 허용) 상태다 - gCurrentTask를
    // 지우기 전에 큐에 먼저 넣으면, 그 사이 끼어든 스케줄러 틱이
    // "지금 실행 중인 Task"와 "막 큐에 들어온 Task"를 같은 것으로
    // 보고 pickNext()로 자기 자신을 다시 뽑아버릴 수 있다 - 침습적
    // next 포인터가 자기 자신을 가리키며 큐가 깨지고, 아직 완성되지
    // 않은 이 kContextSwitch 준비 상태 위에서 또 다른 kContextSwitch가
    // 겹쳐 실행되며 스택이 망가진다(실측으로 발견). runLoop()의 같은
    // 종류 경쟁과 동일한 이유로 cli를 쓴다 - 여기서 끈 인터럽트는
    // kContextSwitch의 pushfq를 통해 이 Task 자신의 저장된 상태에만
    // 반영되고, 나중에 이 Task가 다시 선택될 때 그 저장된 RFLAGS
    // (IF=1)로 복원되므로 재개 이후로 새어 나가지 않는다 - 그래서
    // 재개 후 별도로 sti할 필요가 없다.
    asm volatile("cli");
    const uint32_t coreIndex = currentCoreIndex();
    Task* current = gCurrentTask[coreIndex];
    if (!current) {
        asm volatile("sti");
        return;  // idle 컨텍스트에서 잘못 호출된 경우 - 할 일 없음
    }
    gCurrentTask[coreIndex] = nullptr;
    enqueue(coreIndex, current);
    kContextSwitch(&current->savedRsp, gIdleSavedRsp[coreIndex]);
    // runLoop이 이 Task를 다시 고를 때까지 여기서 멈춰 있다가, 다시
    // 선택되면 이 지점부터(인터럽트 다시 허용된 채로) 재개된다.
}

void Scheduler::parkCurrent() {
    // yieldCurrent()와 똑같은 이유로 cli - gCurrentTask를 지우기 전에
    // 상태만 Blocked로 바꾸면, 그 사이 낀 스케줄러 틱이 이 Task를
    // "아직 실행 중"으로 보고 pickNext()가 (큐에 없으니 이 Task 본인은
    // 아니지만) 다른 전환을 시도하다가 gCurrentTask가 가리키는 대상과
    // 어긋난 상태로 kContextSwitch를 부를 위험을 없앤다.
    asm volatile("cli");
    const uint32_t coreIndex = currentCoreIndex();
    Task* current = gCurrentTask[coreIndex];
    if (!current) {
        asm volatile("sti");
        return;  // idle 컨텍스트에서 잘못 호출된 경우 - 할 일 없음
    }
    gCurrentTask[coreIndex] = nullptr;
    current->state = TaskState::Blocked;
    // yieldCurrent()와의 유일한 차이 - 어느 큐에도 넣지 않는다. 다시
    // 실행되려면 누군가 scheduleImmediate()/enqueue()로 명시적으로
    // 큐에 넣어야 한다(그 시점엔 이 Task가 어느 큐에도 없다는 게
    // 보장되므로 이중 스케줄링 걱정이 없다).
    kContextSwitch(&current->savedRsp, gIdleSavedRsp[coreIndex]);
    // 누군가 깨워 runLoop이 이 Task를 다시 고를 때까지 여기서 멈춰
    // 있다가, 다시 선택되면 이 지점부터(인터럽트 다시 허용된 채로)
    // 재개된다.
}

void Scheduler::retireCurrentTask() {
    // yieldCurrent()/parkCurrent()와 같은 이유로 cli - gCurrentTask를
    // 지우기 전에 clean-up 큐에 먼저 넣으면, 그 사이 낀 스케줄러 틱이
    // 이 Task를 "아직 실행 중"으로 오인해 존재하지 않는 전환을
    // 시도할 위험이 있다.
    asm volatile("cli");
    const uint32_t coreIndex = currentCoreIndex();
    Task* current = gCurrentTask[coreIndex];
    if (!current) {
        // idle 컨텍스트에서 잘못 호출된 경우 - 이론상 도달 불가(이
        // 함수는 항상 kTaskFallingToEnd -> kTaskOnFallingToEnd를 거쳐
        // "지금 실행 중이던 Task 자신"의 흐름에서만 불린다)지만,
        // [[noreturn]] 계약을 지키기 위해 방어적으로 무한 대기한다.
        asm volatile("sti");
        for (;;) {
            asm volatile("hlt");
        }
    }
    gCurrentTask[coreIndex] = nullptr;
    current->state = TaskState::Zombie;
    // parkCurrent()와 달리 "누군가 깨워주길" 기다리는 게 아니라 다시는
    // 선택되지 않는다 - 이 Task의 커널 스택은 지금 이 kContextSwitch
    // 호출이 실제로 다른 스택으로 넘어가야(=더 이상 이 스택 위에서
    // 실행되지 않게 되어야) 비로소 안전하게 회수할 수 있으므로, 회수
    // 자체는 runLoop()이 idle 컨텍스트(다른 스택) 위에서 이 큐를
    // 드레인하며 나중에 처리한다.
    gCleanupQueues[coreIndex].pushBack(current);
    kContextSwitch(&current->savedRsp, gIdleSavedRsp[coreIndex]);
    // 이 지점으로 다시는 돌아오지 않는다(current는 이미 Zombie로
    // 어느 스케줄 큐에도 없어 다시 뽑힐 수 없다) - kAsyncTaskEntryWrapper
    // 와 동일한 패턴의 방어적 무한 루프.
    for (;;) {
    }
}

void Scheduler::disablePreemption() {
    ++gPreemptDisableCount[currentCoreIndex()];
}

void Scheduler::enablePreemption() {
    uint32_t& count = gPreemptDisableCount[currentCoreIndex()];
    if (count > 0) {
        --count;
    }
}

}  // namespace kernel

// context_switch.S의 kTaskFallingToEnd(entry가 반환해 Task 실행이
// 자연 종료되는 지점)가 호출한다 - PL-2D3184BC "Task 종료 프로토콜"
// (QU-26F9420E 설계자 답변, 2026-09-14)의 두 분기를 그대로 구현한다.
// 이 함수 자체가 반환하면(User-Level 분기) 호출부가 이어서 hlt
// 루프로 들어간다 - Kernel-Level 분기(retireCurrentTask())는 절대
// 반환하지 않는다.
extern "C" void kTaskOnFallingToEnd() {
    kernel::Task* self = kernel::Scheduler::currentTask();
    if (!self) {
        return;  // 이론상 도달 불가 - 방어적으로 그냥 hlt 루프로
    }
    if (self->isUserLevel) {
        // User-Level로 격하된 Task(설계자 지시 1번) - 자기종료
        // syscall을 wait 없이 제출만 하고 반환한다. 아직 이 endpoint에
        // 등록된 핸들러가 없어(프로세스 모델 미착수) submit이 항상
        // 실패로 끝나지만, 그 실패 자체를 이 지점에서 신경 쓸 필요가
        // 없다 - 어차피 바로 이어서 hlt 루프로 들어가 다시는 실행되지
        // 않을 Task이기 때문이다.
        kernel::Syscall::submit(kernel::kSyscallEndpointSelfTerminate, nullptr);
        return;
    }
    // Kernel-Level Task가 계속 커널에 머물러 있는 경우(설계자 지시
    // 2번, 지금 이 프로젝트의 모든 Task가 해당) - 절대 돌아오지 않는다.
    kernel::Scheduler::retireCurrentTask();
}
