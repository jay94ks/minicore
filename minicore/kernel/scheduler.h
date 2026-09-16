#ifndef MINICORE_KERNEL_SCHEDULER_H
#define MINICORE_KERNEL_SCHEDULER_H

#include "interrupt_frame.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "task.h"

namespace kernel {

// 스케줄러 전용 LAPIC 주기 타이머 벡터 - HPET 기반 Timer::tickCount()
// (전역 시각)와는 완전히 독립된 시간원이다(PL-2D3184BC 7단계 - "선점
// 결정은 코어별 독립 LAPIC 타이머가 각자 담당한다"). 0x22(HPET)/
// 0x23(레거시 PIT)과 안 격치는 다음 동적 벡터.
constexpr uint32_t kSchedulerTickVector = 0x24;

// 1퀵텀 = 1틱(PL-2D3184BC 8절 - "지금은 기존 100Hz/10ms 틱을 그대로
// 1퀵텀=1틱으로 쓴다. 나중에 실측하며 조정할 수 있도록 하드코딩하지
// 말고 변수/상수 하나로 노출").
constexpr uint32_t kSchedulerTickHz = 100;

// PL-2D3184BC 4단계 - 코어별 개별 큐(DS-D4E5C451이 이미 확정한 상위
// 구조). 큐 자체는 Task::next 침습적 포인터를 재사용하는 단일 연결
// 리스트(FIFO)다. 지금은 Spinlock 기반 폴백만 구현한다(계획 지시대로
// - 이걸로 먼저 스케줄러 흐름을 검증한 뒤 lock-free 버전으로 교체/
// 비교할 예정, PL-2D3184BC 10번 항목 참고 - 아직 안 함).
class TaskQueue {
public:
    // 큐 꿀리에 넣는다(일반 스케줄링 - enqueue).
    void pushBack(Task* task);

    // 큐 머리에 넣는다(PL-2D3184BC 8-1 "즉시 스케줄링" 전용 -
    // Scheduler::scheduleImmediate가 호출한다. 우선순위/RT 클래스를
    // 무시하고 다음 popFront에서 바로 나가게 한다).
    void pushFront(Task* task);

    // 머리에서 하나 꾼다 - 비어 있으면 nullptr.
    Task* popFront();

    // 진단/휴리스틱 용도(정확한 스냵샷이 필요하면 호출부가 락을
    // 별도로 잡아야 함 - 지금은 그런 호출부 없음).
    bool isEmpty() const;

    // Push/Pull 로드밸런싱(PN-04D6197A, SP-9525C4C0 §2.1)의 근사
    // 큐 길이 - 정확한 값일 필요 없다(대략 얼마나 밀렸는지만 알면
    // 충분). pushBack/pushFront/popFront마다 락 임계구역 밖에서
    // 원자적으로 갱신되므로, 다른 코어가 동시에 값을 바꾸는 중에
    // 읽어도(±1~2 오차) 로드밸런싱 판단에 지장이 없다.
    uint32_t approxLength() const;

private:
    Spinlock _lock;
    Task* _head = nullptr;
    Task* _tail = nullptr;
    AtomicU32 _approxLength;
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

    // 이 코어 큐에서 다음에 실행할 Task를 꾼다(즉시 스케줄링 큐 ->
    // RT 큐 -> 일반 큐 순) - 셋 다 비어 있으면 nullptr.
    static Task* pickNext(uint32_t coreIndex);

    // BSP/AP 각자 자기 코어에서 한 번씩 호출한다(SP-0666DB3C §12.4-1,
    // PN-25587A7D) - Gdt::loadTssForThisCore()/SyscallFastPath::
    // initForThisCore()와 같은 자리(kmain.cpp/smp.cpp). RDTSCP를
    // 지원하면(CPUID.80000001H:EDX[27]) 이 코어의 진짜 인덱스를
    // IA32_TSC_AUX에 심어 currentCoreIndex()가 이후 그 MSR을 rdtscp로
    // 즉시 읽기만 하면 되게 한다 - 미지원 CPU에서는 아무 것도 하지
    // 않고 currentCoreIndex()가 계속 기존 선형 스캔으로 동작한다.
    static void initCoreIndexForThisCore();

    // 이 코어의 Acpi 인덱스. RDTSCP 지원 시(initCoreIndexForThisCore가
    // 감지) IA32_TSC_AUX를 rdtscp로 읽는 O(1) 경로, 아니면 Lapic::id()를
    // Acpi::cpuApicId(i)와 대조해 역산하는 기존 O(코어 수) 선형 스캔
    // 폴백(gdt.cpp의 loadTssForThisCore와 같은 패턴). 어느 경로든
    // Acpi::init()/Lapic::init() 이후에만 호출 가능.
    static uint32_t currentCoreIndex();

    // 이 코어 전용 LAPIC 주기 타이머(kSchedulerTickVector)를 콜다 -
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
    // 명시적으로 깨우기 전까지는 절대 다시 뿑히지 않는다. 원칙대로
    // 이 함수 자체는 "범용 공개 API"가 아니라 기능별 API가 내부에서만
    // 쓰섬 한다(설계 문서 6절) - 첫 소비자는 AsyncReactor(async_task.h,
    // 할 일이 없을 때 파킹) - 깨우는 쪽은 별도 API를 두지 않고 이미
    // 있는 scheduleImmediate()를 그대로 쓴다(파킹된 Task는 어느 큐에도
    // 없으므로 이중 스케줄링 걱정 없이 안전하게 즉시 큐에 넣을 수 있다).
    static void parkCurrent();

    // PL-2D3184BC "Task 종료 프로토콜"(설계자 지시, QU-26F9420E 답변
    // 2번, 2026-09-14) - kTaskFallingToEnd(context_switch.S, 예전
    // kTaskStartTrampoline_halt)가 "Kernel-Level Task가 계속 커널에
    // 머물러 있는" 경우(Task::isUserLevel == false)에 호출한다. 이
    // Task를 Zombie로 표시해 스케줄러에서 완전히 뚀어내고(다시는
    // pickNext에 뿑히지 않음) clean-up 큐에 등록한 뒤 다음 Task(또는
    // idle)로 영구히 전환한다 - **절대 돌아오지 않는다**. 자기 자신의
    // 커널 스택을 아직 쓰고 있는 도중(이 함수 자체가 그 스택 위에서
    // 실행 중)이라 이 자리에서 스택을 직접 회수할 수 없다 - 실제
    // 회수(PageFrameAllocator::freeOrder)는 runLoop()이 idle 컨텍스트
    // (다른 스택) 위에서 이 큐를 드레인하며 나중에 처리한다.
    [[noreturn]] static void retireCurrentTask();

    // PN-71C3D483/QU-84E5B3D5(설계자 답변, 2026-09-15 - "새
    // Scheduler::retireTask(Task*) API 추가") - retireCurrentTask()와
    // 달리 **호출자 자신이 아닌, 이미 이 코어에서 실행 중이 아닌**
    // 임의의 Task를 정리한다. 첫 소비자는 self-terminate 핸들러
    // (scheduler.cpp의 SelfTerminateHandler) - User-Level로 격하된
    // Task가 자연 종료(kTaskOnFallingToEnd)되며 스스로를 Zombie로
    // 표시하고 hlt 루프로 떨어진 뒤, 리액터가 비동기로(다른 Task의
    // 실행 흐름에서) 이 함수를 불러 대신 정리한다.
    //
    // **왜 이게 안전한가(핵심 불변조건)**: 한 코어에서는 항상 정확히
    // 하나의 Task만 실행된다 - 이 함수가 호출되고 있다는 사실 자체가
    // "지금 이 코어의 currentTask는 호출자(예: 리액터)"라는 뜻이고,
    // 따라서 target은 이미 그 이전에 반드시 스위칭되어 나간 상태다.
    // 다만 target이 스위칭되어 나간 뒤 **다시 pickNext에 뿑혀 재실행
    // 되지 않는다는 보장**은 이 함수만으로는 안 나온다 - 그래서
    // 호출부가 target을 스위칭해 나가기 전에(예: kTaskOnFallingToEnd가
    // Syscall::submitDetached보다 먼저) 반드시 `target->state =
    // TaskState::Zombie`로 표시해 둔야 한다 - `Scheduler::onTick()`이
    // Zombie 상태의 outgoing task는 라운드로빈 재삽입(enqueue) 자체를
    // 건너뛰므로, 한 번 Zombie로 표시되고 스위칭되어 나간 Task는 그
    // 뒤로 다시는 어느 큐에도 들어가지 않는다(v1 - 코어 간 이관 없음,
    // target의 마지막 실행 코어가 항상 currentCoreIndex()와 같다는
    // 전제도 이래서 성립한다).
    //
    // retireCurrentTask()와 달리 자기 자신을 다음 Task로 전환할 필요가
    // 없다(target은 이미 실행 중이 아니므로 kContextSwitch 불필요) -
    // **커널 스택을 이 함수 안에서 바로 회수한다**(PageFrameAllocator::
    // freeOrder, 지연 큐 없음). retireCurrentTask()가 자기 자신의 스택
    // 위에서 실행 중이라 회수를 runLoop()의 idle 컨텍스트로 미뤄야 하는
    // 것과 달리, target은 이미 다른 스택(호출자 자신의 것) 위에서
    // 실행 중인 이 함수가 호출되고 있으므로 즉시 회수해도 안전하다.
    //
    // **[개정, PN-645CF608 Resurrect, 2026-09-15]** 예전엔
    // retireCurrentTask()와 같은 지연 회수 큐(gCleanupQueues)를
    // 재사용했으나, `SelfTerminateHandler::onExec`이 이 함수 직후 같은
    // Process/UserThread 정적 인스턴스를 재사용해 즉시 재스폰
    // (`ProcessStartFlags::resurrect`, SP-EAB162FC §6)할 수 있게 되면서
    // 문제가 생겼다 - 지연 큐가 실제로 드레인되기 전에 재스폰이
    // `Task::init()`으로 같은 Task 객체의 `kernelStackPhys`/
    // `kernelStackSize`를 새 값으로 덮어써 버리면, 나중에 드레인되는
    // 큐 항목이 이미 땜 용도로 쓰이고 있는 새 스택의 물리 프레임을
    // (엉녡한 크기로) 잘못 반납하는 use-after-reuse 버그가 된다. 즉시
    // 회수로 바꾸면 재스폰이 그 값을 덮어쓰기 전에 이미 안전하게
    // 반납이 끝나 있으므로 이 위험이 원천적으로 없어진다.
    static void retireTask(Task* task);

    // 선점 비활성화 카운터(공개 API, PL-2D3184BC 8단계) - 인터럽트
    // 자체는 막지 않는다(onTick이 이 카운트를 보고 Task 전환만
    // 보류한다) - Slab 할당자(SP-D7013B26)의 PreemptionGuard가 코어별
    // 매거진을 보호하는 데 재사용한다. 중첩 호출 가능(카운터 방식).
    static void disablePreemption();
    static void enablePreemption();

    // #NM(Device Not Available, 벡터 7) 트랩 핸들러(SP-83A07867 §8,
    // PN-F258698E) - idt.cpp의 kIsrHandler가 벡터 7을 이 함수로 그대로
    // 넘긴다. 디스패치마다 kSyncFpu가 세워 둔 CR0.TS 때문에, 이 코어의
    // 현재 Task가 실제로 FPU/SSE 명령을 처음 실행하는 순간에만 걸린다 -
    // 직전 FPU 소유자(있다면)를 FXSAVE로 내보내고, 이 Task 자신의
    // 상태를 FXRSTOR(처음이면 FNINIT)로 불러온 뒤 CLTS로 트랩을 풀고
    // 소유권을 넘긴다.
    static void handleFpuTrap();
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
