#ifndef MINICORE_KERNEL_SCHEDULER_H
#define MINICORE_KERNEL_SCHEDULER_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "task.h"

namespace kernel {

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
    // 아님) - Acpi가 매긴 순서 그대로 쓴다.
    static void enqueue(uint32_t coreIndex, Task* task);

    // PL-2D3184BC 8-1 - RT 클래스보다도 먼저 즉시 실행시켜야 하는
    // 긴급 경로(비동기 프레임워크의 완료 통지 등) 전용. 일반
    // enqueue와 분리된 별도 API로 남용을 막는다.
    static void scheduleImmediate(uint32_t coreIndex, Task* task);

    // 이 코어 큐에서 다음에 실행할 Task를 꺼낸다 - 비어 있으면 nullptr.
    static Task* pickNext(uint32_t coreIndex);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SCHEDULER_H
