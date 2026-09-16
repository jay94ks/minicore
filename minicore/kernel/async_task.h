#ifndef MINICORE_KERNEL_ASYNC_TASK_H
#define MINICORE_KERNEL_ASYNC_TASK_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"

namespace kernel {

struct Task;  // 포인터로만 참조(waitingTask) - 전체 정의는 task.h

// 커널 전용 비동기 프레임워크(SP-F682B889, 확정) - kernel::Task보다
// 훨씬 가벼운 전용 스택을 쓰는 스케줄링 가능 단위. Task와 같은
// 소프트웨어 컨텍스트 전환(kContextSwitch)을 그대로 재사용한다(구조가
// 거의 동일 - savedRsp 오프셋 0 고정, task.h의 Task와 같은 불변조건).
using AsyncTaskSubjectCode = uint32_t;   // 어느 AsyncTaskHandler에 속하는지
using AsyncTaskManageCode = uint64_t;    // 그 작업 주체 안에서 이 인스턴스를 식별하는 관리 코드

// Cancelled(PN-40E976F2) - 이 AsyncTask를 기다리던 UserThread가
// 완료/실패보다 먼저 죽어, 결과를 가져갈 사람이 아무도 남지 않았을 때
// 전이하는 상태. onExec을 실행/재개하지 않고 바로 AsyncTaskHandler::
// onCancel만 호출한 뒤 프레임워크가 자원을 반납한다(AsyncReactor::
// drainOnce, scheduler.cpp의 SelfTerminateHandler::onExec 참고).
enum class AsyncTaskState { Ready, Running, Suspended, Completed, Failed, Cancelled };

// 전용 스택 크기 - PL-1E247831이 "정확한 크기는 실측 확정"으로 남겨둔
// 값(v1 시작값 4KiB, GenericSlabAllocator::alloc이 2048B 초과 요청을
// PageFrameAllocator 직행 경로(Order 0)로 자동 위임하므로 그대로 재사용
// 가능하다(페이지 하나를 그대로 받는다).
constexpr uint64_t kAsyncTaskStackSize = 4096;

struct AsyncTask {
    // context_switch.S의 kContextSwitch/kTaskStartTrampoline이 이
    // 오프셋(항상 첫 필드)을 그대로 참조한다 - task.h의 Task와 동일한
    // 불변조건.
    uint64_t savedRsp = 0;

    uint64_t stackBase = 0;  // GenericSlabAllocator가 준 가상주소(해제 시 필요)
    AsyncTaskState state = AsyncTaskState::Ready;

    AsyncTaskSubjectCode subjectCode = 0;
    AsyncTaskManageCode manageCode = 0;
    void* args = nullptr;  // 생성/해제 전부 해당 AsyncTaskHandler 책임(SP-F682B889 §3.1)

    AtomicPtr<AsyncTask> next;  // AsyncReactor 실행 큐용 침습적 다음-포인터

    // 이 AsyncTask가 Completed/Failed에 도달하면 리액터가 이 Task를
    // scheduleImmediate로 깨워 준다(설정돼 있으면) - 일반 kernel::Task
    // 하나가 "이 특정 AsyncTask가 끝날 때까지 블로킹"하고 싶을 때 쓰는
    // 범용 훅이다(Syscall 서브시스템의 waitForSyscall이 첫 소비자).
    // 완료 시점의 코어(=이 AsyncTask를 실행한 그 코어)에서 깨우므로,
    // 대기하는 Task도 반드시 같은 코어에서 Scheduler::parkCurrent()로
    // 잠들어 있어야 한다(v1 범위 - 코어 간 이관 없음).
    Task* waitingTask = nullptr;

    // false면 완료(Completed/Failed) 후에도 리액터가 이 AsyncTask
    // 구조체/전용 스택을 자동으로 반납하지 않는다 - 결과를 나중에
    // 소비해야 하는 호출부(예: waitForSyscall)가 직접 반납할 책임을
    // 진다. 기본값 true(기존 "제출하고 잊는" 소비자와 동일하게 자동
    // 정리)라 기존 submit() 호출부의 동작은 그대로 유지된다.
    bool autoFree = true;

    // subjectCode에 등록된 AsyncTaskHandler::onExec을 처음 실행할
    // 준비가 된 상태로 스택을 구성한다(GenericSlabAllocator에서 전용
    // 스택을 확보) - 실패 시(할당 고갈) stackBase가 0으로 남는다,
    // 호출부(submit)가 반드시 확인해야 한다.
    void init(AsyncTaskSubjectCode subjectCode, AsyncTaskManageCode manageCode, void* args);

    // 현재 실행 중인 AsyncTask 자신이 호출 - 리액터 컨텍스트로 복귀해
    // 다음 AsyncTask를 처리하게 한다. 나중에 리액터가 이 AsyncTask를
    // 다시 큐에서 뽑아 재개시키면 이 호출 지점부터 이어진다.
    static void yield();

    // AsyncTask/AsyncCallbackRegistry가 내부적으로 새 AsyncTask를 만들어
    // 등록하고 이 코어의 리액터에 제출하는 진입점 - 실패 시(구조체
    // 또는 전용 스택 확보 실패) nullptr. autoFree=false로 제출하면
    // 리액터가 완료 후에도 반납하지 않는다(호출부가 나중에 결과를
    // 읽고 직접 반납해야 함 - waitForSyscall류의 소비 패턴).
    static AsyncTask* submit(AsyncTaskSubjectCode subjectCode, AsyncTaskManageCode manageCode, void* args,
                             bool autoFree = true);
};

// 작업 주체(기능)별로 구현 - 실행/실패/취소 셋 다 구현 책임을 진다.
// args(작업 주체별 내부 데이터)를 실행 전에 만드는 것도, 실행이
// 끝나거나(onExec) 실패하거나(onFailure) 소유자가 먼저 죽어
// 취소되거나(onCancel) 어느 쪽으로 끝나든 그 해제도 전부 이
// 처리기의 몫이다 - 프레임워크는 관여하지 않는다.
class AsyncTaskHandler {
public:
    virtual ~AsyncTaskHandler() = default;
    virtual void onExec(AsyncTask* task, void* args) = 0;
    // Slab 할당 실패(nullptr) 등 프레임워크 내부 사유를 포함해
    // 실행 자체가 불가능했을 때 호출된다 - 재시도 여부도 이 처리기가
    // 결정한다(예: 여기서 다시 submit).
    virtual void onFailure(AsyncTask* task) = 0;
    // 이 작업의 소유 스레드/프로세스가 완료 전에 종료돼 강제로
    // 취소될 때 호출된다(onExec/onFailure 둘 다와 배타적). **[구현
    // 완료, PN-40E976F2]** 이 UserThread가 종료될 때
    // (scheduler.cpp의 SelfTerminateHandler::onExec) 아직 완료/실패
    // 전인 pendingSyscalls 항목을 전부 AsyncTaskState::Cancelled로
    // 전이시키고, AsyncReactor::reactorTaskEntry가 그 상태를 보면
    // onExec 대신 이 메서드만 호출한다 - args의 해제도 이 호출
    // 안에서 처리기가 직접 책임진다(onExec/onFailure와 동일한 계약).
    virtual void onCancel(AsyncTask* task, void* args) = 0;
};

// 기능별로 부팅/초기화 시 한 번만 등록한다 - 요청이 생길 때마다
// 등록/해제하지 않는다.
class AsyncCallbackRegistry {
public:
    static AsyncTaskSubjectCode registerHandler(AsyncTaskHandler* handler);
    static AsyncTaskHandler* resolve(AsyncTaskSubjectCode subjectCode);
};

// 커널 전용 비동기 프레임워크의 디스패치 계층(SP-F682B889 §3.4/§4,
// 2026-09-16 재구조 - QU-96BBB769/QU-4034561A/QU-3BDEE348 답변,
// PN-FEAAF154) - **더 이상 코어당 전용 kernel::Task가 아니다.** 예전
// (PN-C46DF296까지)에는 리액터가 각 코어에 하나씩 배치되는 전용
// Task로 존재했으나, "리액터가 idle을 흡수한다"는 설계자 지시에 따라
// `Scheduler::runLoop()`의 idle 폴백(코어에 실행할 Task가 없을 때)이
// `drainOnce()`를 직접 호출하는 방식으로 전면 교체됐다 - 새 kernel::
// Task/전용 스택/park-wake 프로토콜이 전혀 없다.
class AsyncReactor {
public:
    // 전역 1회(BSP에서만, Idt::init() 이후) - "다른 Task가 실행
    // 중일 때"의 즉시 개입 경로(§4 (C), async_task.cpp의
    // kAsyncDrainVector) IDT 벡터를 등록한다. 코어별 상태는 전혀
    // 없다(gExecQueues/gPreemptiveQueues는 이미 정적 배열) - AP는
    // 더 이상 이 클래스를 위해 아무것도 호출할 필요가 없다.
    static void init();

    // 이 코어의 실행 큐(선점 큐 우선)에서 정확히 하나를 꺼내 실행/
    // 재개하거나(kContextSwitch), 만료된 지연 타이머(DelayedExecutionQueue,
    // SP-F15B4A63)를 처리한다 - 할 일을 하나 처리했으면 true, 정말
    // 아무 것도 없었으면 false(그 결과에 따라 호출부가 계속 반복할지
    // 결정한다). **재진입 방지**: 이미 이 코어에서 드레인이 진행 중이면
    // (runLoop()의 인라인 호출이든 §4 (C) 경로의 IPI 핸들러든) 즉시
    // false를 반환한다 - 그렇지 않으면 AsyncTask의 kContextSwitch
    // 재개 지점(gReactorSavedRsp[coreIndex])을 두 실행 흐름이 동시에
    // 덮어쓸 수 있다(같은 코어 안에서 인터럽트로만 발생 가능한 중첩 -
    // 새로 큐잉된 항목은 바깥쪽에서 이미 진행 중인 호출이 이어서
    // 처리하므로 유실되지 않는다). `Scheduler::runLoop()`의 idle
    // 분기와 §4 (C)의 IPI 핸들러(kAsyncDrainVector) 둘 다 이 함수를
    // 호출한다.
    static bool drainOnce(uint32_t coreIndex);

    // 인터럽트 컨텍스트에서 호출 가능 - 완료(또는 새로 생성)된
    // AsyncTask를 이 코어의 실행 큐에 push한다.
    //
    // **[재설계, 2026-09-16, QU-3BDEE348 답변 - 하이브리드 (A)+(C)]**
    // 리액터가 더 이상 Task가 아니므로 "파킹돼 있으면 강제 스케줄링"
    // 개념 자체가 사라졌다 - 대신:
    // - **preemptive=false → (A)**: 그냥 큐에 넣기만 한다. 이 코어가
    //   다음에 idle 분기(pickNext()==nullptr)에 도달하는 순간 자연히
    //   처리된다(그 사이 다른 Task가 실행 중이었다면 그 Task가 곧
    //   블로킹되거나 스스로 끝나 idle로 돌아오는 게 일반적인 패턴 -
    //   실측 근거: `Syscall::submit()`이 유일한 "다른 Task 실행 중"
    //   실사용처이고, 그 직후 관례상 `Syscall::wait()`로 곧 자기
    //   자신을 파킹한다).
    // - **preemptive=true → (C)**: 선점 큐에 넣은 뒤 이 코어 자신에게
    //   `kAsyncDrainVector` IPI를 보낸다(async_task.cpp) - 인터럽트가
    //   다시 켜지는 즉시(대개 이 함수의 호출부가 반환하는 시점) 그
    //   ISR이 `drainOnce()`를 큐가 빌 때까지 반복 호출해 즉시 처리를
    //   보장한다. exclusivePreemptive Channel(SP-00CA7175 Tier B)
    //   핸드셰이크 완료 등 지연시간이 중요한 경로가 이 값을 넘긴다.
    static void submitCompletion(AsyncTask* task, bool preemptive = false);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_ASYNC_TASK_H
