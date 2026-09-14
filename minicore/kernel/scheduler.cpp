#include "scheduler.h"

#include "acpi.h"
#include "interrupt_frame.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"

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
    // 물리 LAPIC 타이머는 코어당 하나뿐이다 - HPET가 있으면
    // Timer::init()이 LAPIC을 아예 건드리지 않으므로(timer.cpp)
    // 여기서 그대로 독점할 수 있다. HPET가 없는 폴백 환경에서는
    // Timer가 이미 kTimerVector로 이 하드웨어를 쓰고 있어 이 호출이
    // 그걸 덮어써 버린다는 미해결 설계 공백이 있다 - DC-0CC88ABB로
    // 등록해 설계자 확인 대기 중이다. 지금 QEMU 개발 환경은 항상
    // HPET가 있어 실측 경로에는 영향이 없다.
    Lapic::startPeriodicTimer(kSchedulerTickVector, kSchedulerTickHz);
}

void Scheduler::onTick(InterruptFrame*) {
    // 가장 먼저 EOI - 이 아래서 Task 전환이 일어나면 이 함수 호출은
    // 그 Task가 다시 스케줄될 때까지 "반환"하지 않는다(kContextSwitch
    // 가 콜스택 깊숙이 매달린 채로 남는다) - EOI를 미루면 그 사이
    // 이 코어에 다음 스케줄러 틱이 아예 전달되지 않는다.
    Lapic::sendEoi();

    const uint32_t coreIndex = currentCoreIndex();
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
