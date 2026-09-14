#ifndef MINICORE_KERNEL_SCHEDULER_H
#define MINICORE_KERNEL_SCHEDULER_H

#include "interrupt_frame.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "task.h"

namespace kernel {

// 스케줄러 전용 LAPIC 주기 타이머 벡터 - HPET 기반 Timer::tickCount()
// (전역 시각)와는 완전히 독립된 시간원이다(PL-2D3184BC 7단계 - "선점
// 결정은 코어별 독립 LAPIC 타이머가 각자 담당"). 0x22(HPET)/
// 0x23(레거시 PIT)과 안 겹치는 다음 동적 벡터.
constexpr uint32_t kSchedulerTickVector = 0x24;

// 1퀀텀 = 1틱(PL-2D3184BC 8절 - "지금은 기존 100Hz/10ms 틱을 그대로
// 1퀀텀=1틱으로 쓴다. 나중에 실측하며 조정할 수 있도록 하드코딩하지
// 말고 변수/상수 하나로 노출").
constexpr uint32_t kSchedulerTickHz = 100;

// PL-2D3184BC 4단계 - 코어별 개별 큐(DS-D4E5C451이 이미 확정한 상위
// 구조). 큐 자체는 Task::next 침습적 포인터를 재사용하는 단일 연결
// 리스트(FIFO)다. 지금은 Spinlock 기반 폴백만 구현한다(계획 지시대로
// - 이걸로 먼저 스케줄러 흐름을 검증한 뒤 lock-free 버전으로 교체/
// 비교할 예정, PL-2D3184BC 10번 항목 참고 - 아직 안 함).
class TaskQueue {
public:
    // 큐 꼬리에 넣는다(일반 스케줄링 - enqueue).
    void pushBack(Task* task);

    // 큐 머리에 넣는다(PL-2D3184BC 8-1 "즉시 스케줄링" 전용 -
    // Scheduler::scheduleImmediate가 호출한다. 우선순위/RT 클래스를
    // 무시하고 다음 popFront에서 바로 나가게 한다).
    void pushFront(Task* task);

    // 머리에서 하나 꺼낸다 - 비어 있으면 nullptr.
    Task* popFront();

    // 진단/휴리스틱 용도(정확한 스냅샷이 필요하면 호출부가 락을
    // 별도로 잡아야 함 - 지금은 그런 호출부 없음).
    bool isEmpty() const;

private:
    Spinlock _lock;
    Task* _head = nullptr;
    Task* _tail = nullptr;
};

// 코어별 TaskQueue를 관리한다(DS-D4E5C451 "코어별 개별 큐"). Acpi::init()
// 이후에 init()을 호출해야 한다(코어 수를 Acpi::cpuCount()에서 얻음).
// 아직 이 클래스를 실제로 소비하는 디스패치 루프는 없다(LAPIC 틱
// 기반 선점=5단계, 라운드로빈=6단계에서 연결) - 지금은 큐 자료구조
// 자체만 검증된 상태다.
class Scheduler {
public:
    static void init();

    // coreIndex는 Acpi::cpuApicId(index)와 같은 논리 인덱스(APIC ID
    // 아님) - Acpi가 매긴 순서 그대로 쓴다. taskClass가 RealTime이면
    // RT 전용 큐로, 아니면 일반 큐로 들어간다(6단계 - RT는 일반보다
    // 항상 먼저 pickNext된다).
    static void enqueue(uint32_t coreIndex, Task* task);

    // PL-2D3184BC 8-1 - RT 클래스보다도 먼저 즉시 실행시켜야 하는
    // 긴급 경로(비동기 프레임워크의 완료 통지 등) 전용. 일반
    // enqueue와 분리된 별도 API로 남용을 막는다.
    static void scheduleImmediate(uint32_t coreIndex, Task* task);

    // 이 코어 큐에서 다음에 실행할 Task를 꺼낸다(즉시 스케줄링 큐 ->
    // RT 큐 -> 일반 큐 순) - 셋 다 비어 있으면 nullptr.
    static Task* pickNext(uint32_t coreIndex);

    // 이 코어의 Acpi 인덱스 - Lapic::id()를 Acpi::cpuApicId(i)와
    // 대조해 역산한다(gdt.cpp의 loadTssForThisCore와 같은 패턴).
    // Acpi::init()/Lapic::init() 이후에만 호출 가능.
    static uint32_t currentCoreIndex();

    // 이 코어 전용 LAPIC 주기 타이머(kSchedulerTickVector)를 켠다 -
    // BSP/AP 각자 자기 코어에서, Lapic::init() 이후 한 번씩 호출한다.
    static void startTickOnThisCore();

    // idt.cpp가 kSchedulerTickVector 인터럽트마다 호출한다(EOI는 이
    // 함수가 직접, 가장 먼저 보낸다 - 선점 컨텍스트 전환 중에도 다음
    // 틱이 막히지 않아야 하기 때문에 kIsrHandler의 일반적인 "핸들러
    // 반환 후 EOI" 순서를 따르지 않는다, kTimerVector와 같은 특례).
    static void onTick(InterruptFrame* frame);

    // 이 코어의 디스패치 루프 - 절대 반환하지 않는다. kMain/kApMain이
    // 기존 hlt 루프 대신 마지막에 호출한다. 이 코어의 큐가 비어 있는
    // 동안은 sti+hlt로 다음 인터럽트(틱 포함)까지 대기한다.
    [[noreturn]] static void runLoop();

    // 이 코어에서 지금 실행 중인 Task - 없으면(idle) nullptr.
    static Task* currentTask();

    // 협조적 양보 - 현재 Task를 Ready로 다시 큐에 넣고 이 코어의 다음
    // Task(또는 idle)로 전환한다. 호출 시점엔 인터럽트 컨텍스트가
    // 아니어야 한다(일반 Task 실행 흐름에서만 호출).
    static void yieldCurrent();

    // PL-2D3184BC 6단계 - "특정 이유로 블로킹 후 누군가 깨울 때까지
    // 대기"의 범용 내부 프리미티브. yieldCurrent()와 달리 **어느
    // 큐에도 다시 넣지 않는다** - scheduleImmediate()/enqueue()로
    // 명시적으로 깨우기 전까지는 절대 다시 뽑히지 않는다. 원칙대로
    // 이 함수 자체는 "범용 공개 API"가 아니라 기능별 API가 내부에서만
    // 써야 한다(설계 문서 6절) - 첫 소비자는 AsyncReactor(async_task.h,
    // 할 일이 없을 때 파킹) - 깨우는 쪽은 별도 API를 두지 않고 이미
    // 있는 scheduleImmediate()를 그대로 쓴다(파킹된 Task는 어느 큐에도
    // 없으므로 이중 스케줄링 걱정 없이 안전하게 즉시 큐에 넣을 수
    // 있다).
    static void parkCurrent();

    // 선점 비활성화 카운터(공개 API, PL-2D3184BC 8단계) - 인터럽트
    // 자체는 막지 않는다(onTick이 이 카운트를 보고 Task 전환만
    // 보류한다) - Slab 할당자(SP-D7013B26)의 PreemptionGuard가 코어별
    // 매거진을 보호하는 데 재사용한다. 중첩 호출 가능(카운터 방식).
    static void disablePreemption();
    static void enablePreemption();
};

// 진입 시 이 코어의 선점을 비활성화하고 소멸 시 복구하는 RAII 래퍼
// (SP-D7013B26 §2.1) - 이 스코프 안에서는 이 코어가 다른 Task로
// 전환되지 않는다(인터럽트 자체는 계속 처리됨). Slab 할당자의 코어별
// 매거진처럼 "이 코어만 건드린다"는 전제의 lock-free 자료구조를
// 보호하는 데 쓴다.
class PreemptionGuard {
public:
    PreemptionGuard() { Scheduler::disablePreemption(); }
    ~PreemptionGuard() { Scheduler::enablePreemption(); }

    PreemptionGuard(const PreemptionGuard&) = delete;
    PreemptionGuard& operator=(const PreemptionGuard&) = delete;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SCHEDULER_H
