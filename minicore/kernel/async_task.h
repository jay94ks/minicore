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

enum class AsyncTaskState { Ready, Running, Suspended, Completed, Failed };

// 전용 스택 크기 - PL-1E247831이 "정확한 크기는 실측 확정"으로 남겨둔
// 값(v1 시작값 4KiB, GenericSlabAllocator::alloc이 2048B 초과 요청을
// PageFrameAllocator 직행 경로(Order 0)로 자동 위임하므로 그대로 재사용
// 가능 - 버킷화 없이 페이지 하나를 통째로 받는다).
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
    // 취소될 때 호출된다(onExec/onFailure 둘 다와 배타적). **아직
    // 이 경로를 실제로 호출하는 Task/프로세스 종료 절차 자체가
    // 구현돼 있지 않다** - 프로세스 모델이 생길 때 그 종료 시퀀스가
    // 이 메서드를 호출하는 지점을 가져야 한다(SP-04EE2A18 참고).
    virtual void onCancel(AsyncTask* task, void* args) = 0;
};

// 기능별로 부팅/초기화 시 한 번만 등록한다 - 요청이 생길 때마다
// 등록/해제하지 않는다.
class AsyncCallbackRegistry {
public:
    static AsyncTaskSubjectCode registerHandler(AsyncTaskHandler* handler);
    static AsyncTaskHandler* resolve(AsyncTaskSubjectCode subjectCode);
};

// 코어당 전용 리액터 Task - 우선순위 무시 즉시 스케줄링으로 깨어난다.
class AsyncReactor {
public:
    // BSP/AP 각자 자기 코어에서 한 번씩 호출한다(Scheduler::init() 이후,
    // Lapic::init() 이전이든 이후든 무방 - 실제 실행은 Scheduler가
    // 디스패치를 시작한 뒤에나 일어난다). 이 코어의 리액터 Task를
    // 만들어 일반 큐에 최초 1회 넣는다 - 실행되면 즉시 자기 할 일이
    // 없음을 확인하고 파킹한다.
    static void initForThisCore();

    // 코어별로 하나씩 생성되는 리액터 Task의 진입점 - 실행 큐를
    // 드레인하며 각 AsyncTask를 kContextSwitch로 진입/재개한다. 할
    // 일이 없으면 Scheduler::parkCurrent()로 잠든다.
    static void reactorTaskEntry(void* arg);

    // 인터럽트 컨텍스트에서 호출 가능 - 완료(또는 새로 생성)된
    // AsyncTask를 이 코어의 실행 큐에 push하고, 리액터가 파킹돼 있으면
    // Scheduler::scheduleImmediate로 즉시 깨운다(이미 실행 중이면 다음
    // 자기 루프에서 자연히 집어가므로 다시 깨울 필요가 없다).
    static void submitCompletion(AsyncTask* task);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_ASYNC_TASK_H
