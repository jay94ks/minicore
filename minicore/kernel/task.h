#ifndef MINICORE_KERNEL_TASK_H
#define MINICORE_KERNEL_TASK_H

#include "interrupt_frame.h"
#include "libkcont/intrusive_list.h"
#include "libkenv/chunked_list.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "waitable.h"

namespace kernel {

class Waitable;  // WeakPtr<Waitable>로만 참조(Task::blockedOn) - 전체 정의는 waitable.h(SP-0666DB3C §9.2, WaitCancelReason도 여기)
class Process;  // WeakPtr<Process>로만 참조(KernelThread::process) - 전체 정의는 process.h(UserThread::process와 동일한 순환 include 회피 관례)

// [신규, 2026-09-19, PN-0AC554C2 1단계] Task::blockedOn(아래)의
// ChunkedList 청크 용량 - signal.h의 kPendingSignalChunkCapacity와
// 같은 관례(작게 시작, 오늘 기준 유일한 소비자 WaitQueue는 Task당
// 0개 아니면 1개만 채운다 - 실측 후 조정 가능한 구현 세부).
constexpr uint32_t kBlockedOnChunkCapacity = 4;

// SP-0666DB3C §11 - 명시적(explicit) TLS 슬롯 개수. [갱신, 2026-09-18,
// PN-22E5E9E7 항목2] 예전엔 Task::tlsSlots(평범한 배열 필드) 크기였으나,
// 이제 tls.h/tls.cpp의 진짜 `thread_local void* gTlsSlots[kMaxTlsSlots]`
// 크기이자 TlsRegistry::allocateSlot()(tls.h)의 발급 상한이다 - 그
// 배열이 여전히 이 값을 참조하므로(순환 include 회피) 이 값 자체는
// 그대로 Task와 같은 헤더에 둔다(ThreadLocal<T>/TlsRegistry 전체 선언은
// tls.h 참고).
constexpr uint32_t kMaxTlsSlots = 16;  // 실측 후 조정(RM-23F4B687 §4 원칙)

// 스케줄러의 최소 스케줄링 단위(PL-2D3184BC, 설계자 지시, QU-BA001D73,
// 2026-09-14 - "커널 작업은 태스크(Task)라 명명하고, 이걸 사용한다.
// 일반적인 프로세스는 다수의 쓰레드를 가질 수 있으며 각 쓰레드는
// 어떤 코어에서만/코어에서는 실행(불)가능을 결정할 수 있어야한다").
// 프로세스(주소공간+자원 소유)와 Task(실제 스케줄링/실행 단위)는
// 개념적으로 분리되지만, 유저 프로세스가 아직 없어 이 구조체는
// 지금은 커널 전용 실행 흐름만 표현한다 - 유저랜드가 생기면 CR3/
// 유저 스택 등 필드가 추가될 것이다.

enum class TaskState {
    Ready,
    Running,
    Blocked,
    Zombie,
};

enum class TaskClass {
    Normal,
    RealTime,  // QU-77A52430 - 항상 Normal보다 먼저 스케줄링(구현 예정)
    // [신규, 2026-09-19, PN-D47FBB8D] 코어당 정확히 하나씩 존재하는
    // idle/리액터 통합 Task(scheduler.cpp의 gIdleTask[coreIndex]) 전용
    // 태그 - `Scheduler::pickNext()`의 세 큐(Immediate/RealTime/Normal)
    // 어디에도 절대 들어가지 않는다(그 큐들에 들어가면 vruntime=0
    // 고정이라 popMin()이 항상 이 Task를 최우선으로 뽑아 실제 작업을
    // 영원히 굶길 수 있음) - onTick()이 이 값으로 "지금 idle로/에서
    // 전환 중인가"를 판단해 enqueue()/vruntime 계정 대상에서 제외한다.
    Idle,
};

// affinityMask 비트 i가 1이면 코어 i에서 실행 가능 - "이 코어에서만"
// (허용 목록)과 "이 코어에서는 불가"(제외 목록) 둘 다 전체 마스크에서
// 시작해 비트를 세우거나/지우는 것으로 자연스럽게 표현된다.
constexpr uint32_t kTaskAffinityAllCores = 0xFFFFFFFFU;
constexpr uint64_t kTaskDefaultKernelStackSize = 8UL * 1024UL;  // 8KiB(QU-BA001D73)

using TaskEntry = void (*)(void* arg);

// [신규, 2026-09-19, PN-8726CDBD, 설계자 의견] CR0.TS 기반 lazy FPU/SSE
// 컨텍스트 저장 영역(SP-83A07867 §8, PN-F258698E)을 Task 밖으로 뺀
// 얇은 컨테이너 - FXSAVE/FXRSTOR이 요구하는 16바이트 정렬 512바이트
// 블록만 담는다. Task는 이걸 UniquePtr로만 가리켜(기본 nullptr) FPU를
// 안 쓰는 Task(커널 전용 Task 등)는 이 512바이트를 아예 안 갖고
// 다니게 한다 - "이 Task가 FPU를 초기화한 적이 있는가" 판정도
// 별도 bool 없이 포인터 존재 자체(`Task::fpuContext`)로 단순화된다.
struct TaskFpuContext {
    alignas(16) uint8_t buffer[512] = {};
    // shared_ptr.h의 kDestroyAndFree<T> 기본 삭제자 관례(T::destroy()
    // 호출 후 슬랩 반납) - 이 struct는 순수 POD라 정리할 게 없어도
    // 그 관례를 그대로 따르기 위한 빈 구현.
    void destroy() {}
};

// [신규, 2026-09-20, PN-C536F352, RM-23F4B687 - onExec() currentTask()
// 오용 재발(다섯 번) 근본 수정] "지금 이 코어가 실행 중인 Task"(호출
// 시점 전역 상태, `Scheduler::currentTask()`)와 "이 작업을 누가
// 맡겼는가"(제출 시점 정체성)는 구조적으로 다른 개념인데, 둘 다
// `Task*`로 조회 가능해 보이는 게 착시를 유발해 같은 버그가 다섯 번
// 재발했다(`AsyncTask`의 onExec() 계열 콜백은 리액터 자신의 Task
// 컨텍스트에서 실행되므로 제출자와 다르다). `AsyncTask::submitterTask`
// (async_task.h)가 이미 "제출 시점에 캡처해 둔 WeakPtr<Task>로만
// owner를 찾는다"는 안전한 관례를 애드혹으로 구현해 왔는데, 이 타입은
// 그 관례를 이름 있는 재사용 가능한 값 타입으로 일반화한다 - 새로
// 이런 지연 실행 콜백을 작성하는 코드가 매번 같은 필드를 손으로 다시
// 만드는 대신 이 타입을 쓰게 하는 것이 목적("규칙을 기억하기"에서
// "타입을 재사용하기"로).
//
// **`captureCurrent()`가 아니라 `capture(WeakPtr<Task>)`인 이유**:
// 설계안(PN-C536F352)이 처음 스케치한 모양은 인자 없는 정적 팩토리
// (`Scheduler::currentTask()`를 내부에서 알아서 스냅샷)였으나, 실제
// 구현 중 확인한 사실 - `Task` 자신은 `EnableSharedFromThis<Task>`를
// 상속하지 않는다(그 SharedPtr 제어 블록은 파생 클래스, 예:
// `UserThread : public Task, public EnableSharedFromThis<UserThread>`
// 쪽에만 있다 - `syscall.h`의 `weakAsThis()` 참고) - 그래서 "지금의
// `Task*`를 범용적으로 `WeakPtr<Task>`로 바꾸는" 마법 같은 무인자
// 정적 함수는 만들 수 없다. 대신 호출자가 이미 자기 자신의 진짜
// `WeakPtr<Task>`(예: `UserThread::weakAsTask()`)를 손에 쥔 **진짜
// 제출 시점**에 그 값을 그대로 넘기는 형태로 캡처한다 - 의미상으로는
// 스케치의 `captureCurrent()`와 동일(제출 시점 스냅샷, 이후 이
// 팩토리를 지연 실행 컨텍스트에서 다시 부르면 안 됨)하고 실제로
// 구현 가능하다.
//
// **여러 번 호출해도 안전한 이유**: `resolve()`(및 호환용 별칭
// `lock()`)는 자신이 들고 있는 불변 `WeakPtr<Task>`만 원자적으로
// lock()할 뿐 `gCurrentTask[coreIndex]` 같은 코어별 전역 상태를 전혀
// 참조하지 않는다 - 그래서 재진입/동시 호출 모두 그 자체로 안전하고,
// "잘못된 컨텍스트에서 불렀는지" 검출이 애초에 필요 없다.
class TaskOwnerRef {
public:
    TaskOwnerRef() = default;

    // owner - 진짜 제출 컨텍스트(syscall 트랩, AsyncTask::submit() 등)
    // 에서 호출자가 이미 얻어 둔 자기 자신의 WeakPtr<Task>. 이 값을
    // 나중에 지연 실행 컨텍스트(onExec() 등)에서 다시 계산하려 들면
    // 이 타입이 막으려는 바로 그 버그가 재발하므로 절대 그러지 않는다.
    // (task.cpp에 정의 - Scheduler::currentCoreIndex() 참조가 필요한데
    // scheduler.h가 이미 task.h를 include하므로 여기서 scheduler.h를
    // include하면 순환 include가 된다.)
    static TaskOwnerRef capture(WeakPtr<Task> owner);

    // 대상이 이미 죽었으면 빈 SharedPtr(널 아님, 빈 값) - 기존
    // WeakPtr::lock() 관례 그대로 호출자가 매번 유효성을 확인한다.
    SharedPtr<Task> resolve() const { return _ownerTask.lock(); }

    // 호환용 별칭 - `WeakPtr<Task>` 시절부터 있던 `submitterTask.lock()`
    // 관례를 그대로 재사용하는 기존 호출부가 이 타입으로 교체돼도
    // 안 깨지게 한다.
    SharedPtr<Task> lock() const { return resolve(); }

    // 디버깅/방어적 assert 전용 - "지금 이 호출이 정말 소유자 자신의
    // 실행 흐름 위에서 일어났는가"(코어 무관, Task가 그사이 다른
    // 코어로 마이그레이션했을 수도 있음). onExec()의 정상 동작에는
    // 쓰지 않는다(그게 바로 이 타입이 막으려는 버그 클래스이므로).
    // (task.cpp에 정의 - capture()와 같은 순환 include 이유)
    bool isCurrentCoreOwner() const;

private:
    WeakPtr<Task> _ownerTask;
    uint32_t _ownerCoreIndexAtCapture = 0;  // 참고용 - resolve()/lock()이 안 씀
};

// context_switch.S의 kContextSwitch/kTaskStartTrampoline이 이 구조체의
// tcb 오프셋(항상 첫 필드, 오프셋 0)을 그대로 참조한다 - 필드 순서를
// 바꾸려면 그쪽 어셈블리도 같이 확인해야 한다.
struct Task {
    // [갱신, 2026-09-20, PN-81E49523 2단계, 설계자 지시("TaskTcb 자체를
    // Task 구조체에 계속 유지해두고, kContextSwitch에 바로 넘길 수 있는
    // 형태로 유지")] 이 Task의 마지막으로 캡처된 완전한 레지스터
    // 상태(TaskTcb=InterruptFrame, interrupt_frame.h)를 직접 가리키는
    // 포인터 - `kContextSwitch`/`kContextSwitchFromISR`에 그대로
    // 넘겨(추가 변환/복사 없이) 다음 전환의 newTcb 인자로 쓸 수 있다.
    // 실제로 가리키는 위치는 상황에 따라 다르다: 이 Task 자신의 커널
    // 스택 위(`kContextSwitch`가 그 자리에서 조립한 프레임, 협조적
    // 전환), 인터럽트가 만든 진짜 InterruptFrame 그대로(`kContextSwitchFromISR`,
    // 재조립 없이 그 주소만 저장), 또는 `Task::init()`이 미리 지어 둔
    // 가짜 최초 프레임 - 셋 다 모양이 완전히 같아(TaskTcb) 어느 쪽이든
    // 균일하게 재개 가능하다.
    TaskTcb* tcb = nullptr;

    uint64_t kernelStackPhys = 0;  // PageFrameAllocator가 준 물리주소(해제 시 필요)
    uint64_t kernelStackSize = 0;

    // 이 Task의 커널 스택 top(가상주소) - Task::init()이 실제로 어느
    // 방식(direct map 별칭 vs MINICORE_TASK_STACK_GUARD_PAGE 켰을 때의
    // 전용 매핑)으로 스택을 마련했든 상관없이 항상 여기서 정확한 값을
    // 얻을 수 있다(PN-AEA74E1B - 예전엔 kEnterRing3가 direct map 별칭
    // 계산식을 직접 다시 만들어 썼는데, 가드 페이지 켠 빌드에서는 그
    // 계산식 자체가 틀렸다 - 이 필드로 그 중복/오류 가능성을 없앤다).
    uint64_t kernelStackTop = 0;

    // 이 Task가 ring3에서 실행될 때 쓰는 유저 주소공간의 PML4 물리
    // 프레임(PN-63BCFE45) - `isUserLevel`인 Task만 의미가 있다.
    // **왜 필요한가**: CR3는 소프트웨어 컨텍스트 스위칭(kContextSwitch)
    // 이 저장/복원하는 레지스터 집합(콜리세이브+RFLAGS)에 들어있지
    // 않다 - `iretq`도 CR3를 건드리지 않는다. 그래서 이 Task가 ring3
    // 첫 진입(process.cpp의 kEnterRing3) 이후 두 번째로 디스패치될
    // 때는 CR3가 여전히 "그 사이 마지막으로 실행됐던 다른 UserThread"
    // 의 값으로 남아 있다 - `Scheduler::onTick()`이 매 Task-to-Task
    // 전환마다 이 필드를 다시 CR3에 실어야(scheduler.cpp의
    // kSyncCr3ForDispatch - `Scheduler::runLoop()`의 idle 컨텍스트에서는
    // 안전하지 않아 호출하지 않는다) 서로 다른 프로세스가 코드/스택
    // 레이아웃이 실제로 다를 때 즉시 크래시하는 실측 버그를 막는다 -
    // 우연히 코드가 동일한 스레드끼리는 겉보기엔 멀쩡해 보여서(같은
    // 가상주소에 같은 명령이 있으니) 늦게 발견됐다. **아직 완전히
    // 해결되진 않았다** - runLoop()이 이미 한 번 실행된 적 있는
    // UserThread를 idle 상태에서 다시 고르는 경우(yieldCurrent/
    // parkCurrent 경로, 현재는 어떤 ring3 코드도 안 거침)는 여전히
    // CR3가 안 맞을 수 있다 - PN-63BCFE45 참고.
    uint64_t userPml4Phys = 0;

    // [신규, 2026-09-18, PN-22E5E9E7 항목2/3, SP-29D652AA §4.1/§4.4]
    // 이 Task 전용 TCB(Thread Control Block) 주소 - 진짜 컴파일러
    // `thread_local` 변수(tls.h의 `gTlsSlots` 등, .tdata/.tbss 템플릿
    // 인스턴스)에 접근할 때 컴파일러가 생성하는 %fs-상대 코드가 실제로
    // 참조하는 주소다. `Task::init()`(task.cpp)이 링커가 만든 템플릿
    // 경계(`kTlsTemplateStart`/`kTlsTemplateTdataEnd`/`kTlsTemplateEnd`,
    // linker.ld)를 Slab에서 복사해 이 Task만의 인스턴스를 만들고, 그
    // 블록의 **끝 주소**(x86_64 TLS variant II 관례 - FS_BASE가 블록의
    // 끝을 가리키고 개별 변수는 음수 오프셋으로 접근)를 여기 담는다.
    // **왜 userPml4Phys와 같은 자리에 있는가**: CR3와 똑같이 FS_BASE도
    // `kContextSwitch`(콜리세이브+RFLAGS만 저장/복원)도 `iretq`도 건드리지
    // 않는 순수 MSR이라, Task 전환마다 `kSyncFsBase`(scheduler.cpp)가
    // `kSyncCr3`와 정확히 같은 다섯 디스패치 지점에서 다시 실어야 한다
    // (userPml4Phys 문서 주석 참고 - 같은 문제, 같은 해법). 이 값은
    // 템플릿 복사본 바로 뒤에 이어붙인 8바이트 self-pointer 헤더의
    // 주소다(`*reinterpret_cast<uint64_t*>(kernelFsBase) == kernelFsBase`) -
    // [실측 정정, 2026-09-18] 처음엔 `-ftls-model=local-exec`(cmake/
    // toolchain-x86_64.cmake)면 self-pointer 헤더 자체가 필요 없을
    // 거라 봤으나, `gTlsSlots`가 extern이라 Itanium C++ ABI가 강제하는
    // TLS 래퍼 함수(`_ZTW...`, task.cpp의 `kMakeTaskTlsBlock()` 문서
    // 주석 참고)가 `-ftls-model`과 무관하게 항상 FS:0을 역참조해
    // "스레드 포인터 자신"부터 읽으므로, 그 자리에 진짜 self-pointer가
    // 있어야 한다는 것을 QEMU 실측(즉시 페이지 폴트)으로 발견했다.
    uint64_t kernelFsBase = 0;

    // 이 Task가 ring3 첫 진입(process.cpp의 kEnterRing3) 때 점프할
    // 목표 주소/유저 스택 top(PN-D0ED9611) - `isUserLevel`인 Task만
    // 의미가 있다. **왜 여기 있는가**: 예전엔 이 두 값을 `Task::init()`
    // 의 단일 void* arg 슬롯에 실어 넘기려고 전역 인스턴스
    // (process.cpp의 `gRing3EntryParams`) 하나를 공유했는데, 이러면
    // `Process::execImage()`를 첫 번째 프로세스의 값이 아직 소비되기
    // 전(=그 UserThread가 실제로 kEnterRing3까지 실행하기 전)에 두
    // 번째 프로세스 생성을 위해 또 부르면 첫 번째 값이 조용히
    // 덮어써지는 결함이 있었다(실측으로 발견, PN-63BCFE45 조사 중
    // 별도 확인) - userPml4Phys와 똑같은 이유로 이 Task 자신에게
    // 옮겨 담아 프로세스 개수와 무관하게 안전하게 만든다.
    uint64_t ring3EntryPoint = 0;
    uint64_t ring3UserStackTop = 0;

    TaskState state = TaskState::Ready;
    TaskClass taskClass = TaskClass::Normal;
    uint32_t affinityMask = kTaskAffinityAllCores;

    // [제거, 2026-09-20, PN-81E49523 2단계] `hasEverRun`/`entryFn`/
    // `entryArg` 필드는 여기 있었다 - `Scheduler::onTick()`/
    // `onForcedMigration()`이 "next가 한 번도 디스패치된 적 있는지"에
    // 따라 `kContextSwitch`/`kContextSwitchToFreshTask` 중 고르던
    // 시절(PN-414BF822)의 흔적으로, 그 분기 자체가 이제 `kContextSwitchFromISR`
    // 하나로 통일되며 완전히 죽은 필드가 됐다(Task::init()이 짓는 가짜
    // 최초 프레임이 이제 완전한 `TaskTcb`라 별도 entryFn/entryArg 인자
    // 전달 없이 그 프레임 자체가 rbx/r12 자리에 entry/arg를 담아
    // 옮긴다 - context_switch.S 참고). 제거함.

    // [신규, PN-A74871F2, DC-8EA1E7F6/PL-2D3184BC "Task 자료구조" 절이
    // 원래 요구했으나 구현에서 누락됐던 필드 - RM-F2DAFF66 §1-A 발견]
    // 이 Task를 생성한 코어의 NUMA 노드(`Acpi::cpuNumaNode()`, SRAT
    // 없으면 항상 0). `Task::init()`이 생성 시점에 한 번 채우고 이후
    // 안 바뀐다 - 코어 이관이 일어나도 "원래 어디서 태어났는지"는
    // 그대로 남긴다(커널 스택 실제 물리 메모리의 지역성과 일치,
    // `PageFrameAllocator::allocOrder()`가 이미 "현재 코어 노드 우선"
    // 정책이므로 이 필드는 그 사실을 사후에 기록만 할 뿐 새 할당
    // 정책을 추가하지 않는다). Push/Pull 로드밸런싱의 같은-노드-우선
    // 이관 정책(SP-9525C4C0 §2.3, PN-9DDFB774)이 실제로 참고하는
    // 입력값 - 진단용에 그치지 않는다.
    uint32_t numaNode = 0;

    // 이 Task가 ring3 유저 코드를 실행하는 UserThread면 true -
    // Process::execImage()가 그 UserThread 생성 시점에 직접 세팅한다
    // (PN-55D24891). 순수 커널 전용 Task는 항상 기본값 false로 남는다.
    // kTaskFallingToEnd(entry가 반환해 이 Task의 실행이 자연 종료되는
    // 지점, context_switch.S)가 이 플래그로 종료 처리를 분기한다
    // (PL-2D3184BC "Task 종료 프로토콜", QU-26F9420E 설계자 답변,
    // 2026-09-14) - true면 자기종료 syscall만 제출하고 실제 정리는
    // 리액터가 비동기로 Scheduler::retireTask()에 위임(PN-71C3D483),
    // false면 그 자리에서 바로 Scheduler::retireCurrentTask()로
    // 스케줄러에서 완전히 떼어낸다.
    bool isUserLevel = false;

    // [신규, 2026-09-20, SP-43331889 §1, DC-91ABD922/QU-23B339AB] 이
    // Task가 `KernelThread`(devmgr/fs 등 "커널 모드 프로세스"의 실행
    // 단위 - ring0, 커널 자신의 주소공간 공유, ELF 로드 없이 C++ 함수
    // 포인터를 직접 entry로 씀)면 true. `isUserLevel`과 정확히 같은
    // 관례(평범한 bool 플래그 + `static_cast`, `Task`에 가상 함수를
    // 추가하지 않는다 - `tcb`가 항상 오프셋 0이어야 하는 불변조건이
    // vtable 포인터 삽입으로 깨지기 때문, SP-43331889 §4 정정 참고)
    // - `kOwnerProcessOf(Task*)`(process.h)가 `isUserLevel`/
    // `isKernelMode` 둘 다 확인해 `UserThread`/`KernelThread` 중
    // 알맞은 쪽으로 `static_cast`한다. 순수 커널 전용 Task(idle/
    // 리액터 등, Process 소속 없음)는 이 필드도 계속 기본값 false.
    bool isKernelMode = false;

    // 이 Task가 지금 스케줄러의 세 큐(즉시/RT/일반) 중 어딘가에
    // 실제로 들어있는지 - **실측으로 발견한 이중 스케줄링 경쟁의
    // 구조적 방지책**(2026-09-14, Channel IPC 스트레스 테스트,
    // PL-2D3184BC 참고). "이 Task를 이미 어딘가에서 큐에 넣어 둔
    // 시점"과 "그걸 아직 모르는 다른 호출부가 별도로 또
    // enqueue()/scheduleImmediate()를 부르는 시점" 사이의 창은
    // 스케줄러 틱이 끼어들 수 있는 모든 지점에서 원리상 생길 수 있어
    // 하나하나 찾아 막는 방식만으로는 끝이 없다 - Scheduler::enqueue/
    // scheduleImmediate가 push 직전 이 플래그를 확인해 이미 true면
    // 조용히 재삽입을 생략하고, Scheduler::pickNext가 실제로 큐에서
    // 꺼내는 순간 다시 false로 내린다. state(Ready/Running/...)는 이
    // 용도로 못 쓴다 - Task::init() 직후에도 이미 state=Ready라
    // "아직 한 번도 큐에 들어간 적 없음"과 "이미 큐에 있음"을 구분하지
    // 못한다.
    bool inRunQueue = false;

    // 코어별 큐(폴백/lock-free 공용, PL-2D3184BC 4단계)가 쓰는 침습적
    // (intrusive) 다음-포인터 - 이 Task가 큐에 들어있을 때만 유효.
    AtomicPtr<Task> next;

    // [신규, 2026-09-17, SP-B26CDBDD §2/§3, PN-158B6B2F] `gNormalQueues`가
    // `libkcont::OrderedList<Task, TaskVruntimeTraits>`(vruntime 오름차순)
    // 로 바뀌며 필요해진 전용 침습적 링크 - `next`(위, 즉시/RT 큐가 계속
    // 쓰는 단일 연결)와 별개다(`waitQueueLink`가 이미 확립한 것과 같은
    // "서로 다른 컨테이너는 서로 다른 링크 필드" 원칙).
    Node vruntimeLink;

    // [신규, 2026-09-17, SP-B26CDBDD §2.1] 상대 가중치 - 기본값(0)이
    // "기준"이고 음수면 덜, 양수면 더 받는다. `TaskClass::Normal`에만
    // 의미가 있다(RT/Immediate는 공정 스케줄링 개념 자체와 무관 -
    // SP-9525C4C0 §6-항목4와 동일한 구분). vruntime 계산 시에는 이
    // 표면값을 직접 쓰지 않고 `kEffectiveWeightOf(weight)`(scheduler.cpp)
    // 로 항상 양수인 실효 가중치로 변환한 뒤 쓴다.
    static constexpr int32_t kMinTaskWeight = -100;
    static constexpr int32_t kMaxTaskWeight = 100;
    static constexpr int32_t kDefaultTaskWeight = 0;
    int32_t weight = kDefaultTaskWeight;

    // [신규, 2026-09-17, SP-B26CDBDD §2] CFS류 vruntime - 작을수록
    // "덜 받았다"는 뜻이라 다음 `pickNext()`에서 먼저 뽑힌다
    // (`TaskVruntimeTraits::keyOf`가 이 필드를 그대로 정렬 키로 쓴다).
    uint64_t vruntime = 0;

    // [신규, 2026-09-17, SP-B26CDBDD §1] 이 Task가 실제로 Running이었던
    // 스케줄러 틱 누적 수(100Hz) - 순수 진단/계정용, 스케줄링 결정
    // 자체에는 쓰이지 않는다(그 역할은 vruntime이 담당).
    uint64_t cpuTicksUsed = 0;

    // [신규, 2026-09-17, PN-73E61BD1 항목1, SP-FAF768AB §1/§2]
    // `WaitQueue`(Mutex/Semaphore 공유 대기열) 전용 침습적 링크 -
    // 예전엔 위 `next`(스케줄러 큐 전용 필드)를 "파킹된 Task는 어느
    // 스케줄러 큐에도 없으니 재사용 가능하다"는 논리로 이중 용도로
    // 빌려 썼으나, libkcont `List<T, Traits>`(PN-633BF2D8)로 교체하며
    // 전용 필드로 분리했다 - 한 Task가 스케줄러 큐 자료구조와 대기열
    // 자료구조 양쪽에 동시에 속할 일이 없다는 기존 불변조건은 그대로
    // 유지되지만(파킹 중엔 `next`를 스케줄러가 안 씀), 서로 다른
    // 두 개념을 서로 다른 필드로 표현하는 쪽이 더 명확하다(libkcont
    // intrusive_list.h 문서 주석의 "한 T가 서로 다른 리스트 두 개에
    // 동시에 속해야 하면 Node 멤버를 두 개 두면 된다"는 원칙 그대로).
    // 이 Task가 어느 WaitQueue에도 파킹돼 있지 않을 때는 자기 자신을
    // 가리키는 sentinel 상태(Node의 기본값).
    Node waitQueueLink;

    // 이 Task가 지금 무엇에 막혀 파킹돼 있는지(SP-0666DB3C §9.2, 임의
    // 대기 상태를 강제로 끄집어내는 범용 훅) - 파킹 시작 시 그 대기
    // 구조체 자신(WaitQueue 등)이 설정하고, 깨울 때(정상 wakeOne()이든
    // 강제 cancel()이든) 같은 대기 구조체가 자신의 락 아래에서 다시
    // 빈 값으로 되돌린다(§9.6-1 - 정상 웨이크업과 강제 취소 두 경로가
    // 경쟁해도 정확히 한쪽만 성공하도록, 이 필드 정리 자체를 그 락으로
    // 직렬화한다). 대기 중이 아니거나 실행 중이면 빈 WeakPtr.
    //
    // [수정, 2026-09-17, PN-B41D8C0E, DC-21647E46/QU-4E449C65 답변("(B)
    // SharedPtr 별칭 생성자 추가")] 실제 Waitable 구현체(WaitQueue)는
    // Mutex/Semaphore에 임베디드라 자기 컨트롤 블록이 없다 - 그래서 이
    // WeakPtr은 항상 그 WaitQueue를 담고 있는 Mutex/Semaphore의 컨트롤
    // 블록을 별칭(aliasing)으로 공유한다(WeakPtr(SharedPtr<Mutex>,
    // Waitable*) 생성자, shared_ptr.h). **주의**: 그 Mutex/Semaphore가
    // kMakeShared로 만들어지지 않았으면(스택/정적 인스턴스) 별칭을 만들
    // 컨트롤 블록 자체가 없어 이 필드가 항상 빈 WeakPtr로 남는다 - 그래도
    // 파킹/wakeOne/wakeAll 자체는 정상 동작하지만(그건 이 필드가 아니라
    // WaitQueue 내부 연결 리스트로 처리됨), 이 필드를 통한 강제
    // cancel()(§9.5, 시그널 전달)만 조용히 무력화된다 - 시그널로 즉시
    // 깨워야 하는 Mutex/Semaphore는 반드시 kMakeShared로 만들어야 한다.
    //
    // [갱신, 2026-09-19, PN-0AC554C2 1단계, QU-25E1C297 답변("Task가
    // 대기해야 하는 모든 것을 Waitable로 wrapping하여 리스트에 담는
    // 구조로 전환해")] 단일 `WeakPtr<Waitable>`에서 리스트로 승격 -
    // 이 Task가 동시에 여러 Waitable을 기다릴 수 있게 하고(전부
    // 해소돼야 블로킹이 풀리는 AND 의미, wait_queue.h의
    // `kDrainAndCheckBlockedOn` 참고), `Scheduler::onTick()`의 재스케줄
    // 결정 지점이 이 리스트가 비어있지 않으면 Blocked로 전환하는 새
    // 메커니즘의 기반이 된다. **1단계(현재)에서는 오늘까지의 유일한
    // 소비자(WaitQueue)가 여전히 0개 아니면 1개만 채우는 예전과 동일한
    // 불변조건을 그대로 지킨다** - 실제로 여러 엔트리를 동시에 담는
    // 것은 이후 단계(pendingSyscalls/디버그 정지 마이그레이션, #DB
    // wrapping)가 하는 일이다.
    ChunkedList<WeakPtr<Waitable>, kBlockedOnChunkCapacity> blockedOn;

    // WaitQueue(SP-0666DB3C §1/§5-1)가 이 Task를 파킹시킨 코어 - 나중에
    // wakeOne()/cancel()이 Scheduler::scheduleImmediate(parkedCoreIndex,
    // this)로 정확히 그 코어에서 재개시키는 데 쓴다(다른 코어로 옮겨
    // 깨우는 로드밸런싱은 v1 범위 밖, §5-1).
    uint32_t parkedCoreIndex = 0;

    // §9.6-3(설계 문서 pseudocode에 있었으나 미구현이던 부분,
    // PN-71E50394에서 발견/구현) - 이번에 파킹된 동안 강제로 취소됐다면
    // 그 사유, 정상적으로 깨어났다면(또는 아직 파킹 전이면) None.
    // parkCurrentAndUnlock()가 매번 새로 파킹할 때 None으로 리셋하고,
    // cancel()이 실제로 취소할 때만 그 사유로 덮어쓴다 - 재개된 코드가
    // (예: MutexCore::lock()의 재시도 루프) 이 값을 확인해 "정상
    // 재경쟁"과 "강제로 끌려나옴"을 구분할 수 있게 한다. 아직 이 값을
    // 실제로 읽는 호출부는 없다(다음 후속 항목).
    WaitCancelReason lastCancelReason = WaitCancelReason::None;

    // [수정, 2026-09-19, PN-8726CDBD, 설계자 의견] 예전엔 이 자리에
    // `fpuState[512]`(FXSAVE/FXRSTOR 블록, 항상 인라인)와
    // `fpuInitialized`(bool) 두 필드가 직접 있었다 - FPU를 쓰든 안
    // 쓰든 모든 Task가 이 512바이트를 항상 갖고 다니는 구조였다.
    // 이제 `TaskFpuContext`(위 정의)로 분리해 `nullptr`이 기본인
    // `UniquePtr`만 들고 있는다 - 커널 전용 Task처럼 FPU를 아예 안
    // 쓰면 이 포인터가 계속 `nullptr`로 남아 Task 자체가 가벼워지고,
    // "FPU를 초기화한 적이 있는가" 판정도 `fpuContext != nullptr`
    // 확인 하나로 단순해진다. `Scheduler::handleFpuTrap()`(#NM 최초
    // 히트)이 이 포인터가 비어 있으면 그 시점에 슬랩 할당해 채워
    // 넣는 지연 할당 패턴을 그대로 유지한다(기존 지연 저장 원칙과
    // 일관). **memset(0)+init() 관례(placement new 없음, 이 struct
    // 문서 주석 위쪽 참고) 하에서는 이 UniquePtr의 소멸자가 저절로
    // 불리지 않는다** - `UserThread::release()`가 raw
    // `GenericSlabAllocator::free()` 직전에 반드시 `fpuContext.reset()`
    // 을 먼저 호출해 할당된 `TaskFpuContext`를 명시적으로 반납해야
    // 한다(syscall.cpp 참고, 안 하면 Task가 죽을 때마다 512바이트
    // 슬랩 누수).
    UniquePtr<TaskFpuContext> fpuContext;

    // 커널 스택을 새로 할당하고, entry(arg)를 처음 실행할 준비가 된
    // 상태로 초기화한다(트램폴린 스택 프레임 구성) - 스케줄러 큐에
    // 넣는 것은 호출부 책임(아직 스케줄러 자체가 없어 별도 API 없음).
    // Paging::init() 이후에만 호출 가능(direct map 필요).
    void init(TaskEntry entry, void* arg, uint64_t stackSize = kTaskDefaultKernelStackSize);
};

// [신규, 2026-09-20, SP-43331889 §1, DC-91ABD922/QU-23B339AB 확정
// 반영 - §1 2026-09-20 최종 개정, 설계자 답변(QU-ECEE5990, "(B)
// devmgr/fs를 idle/리액터처럼 Process에 전혀 속하지 않는 순수 커널
// Task로 완전히 단순화") 반영] "커널 모드 Task"(devmgr/fs 등을 커널에
// 완전 통합) - `Process` 소속이 전혀 없다(자원그룹 회계/fd 테이블/
// essential+respawn/프로세스 트리 가시성 전부 포기, idle/리액터와
// 완전히 같은 지위 - "ps로 가시될 필요 없는 대상"이라는 설계자
// 의견 그대로). 최초 초안은 `Process` 소속(`process` 필드,
// `Process::execKernelEntry()`)을 유지하려 했으나 이 답변으로
// 폐기됐다 - 대신 자유 함수 `kSpawnKernelThread()`(아래)로 Process
// 없이 직접 띄운다.
//
// `UserThread`(syscall.h)와 상속 패턴(`EnableSharedFromThis`까지
// 포함, `weakAsTask()`/`sharedSelf()`/`ensureSelfRef()`/`allocate()`/
// `release()`/`_selfRef` 전부 `UserThread`와 동일한 이유로 그대로
// 복제 - `submitterTask` 체이닝이 성립하려면 정적/동적 생성 양쪽 다
// 컨트롤 블록이 있어야 한다는 사정이 완전히 같다, syscall.h의
// `UserThread::_selfRef` 문서 주석 참고)은 그대로 같지만, ELF 로드/
// 유저 스택/별도 `pml4Phys`도 없고 이제 `Process` 링크도 없다는 점이
// 다르다. **[구현 중 확인, 2026-09-20] `Task::init(entry, arg)`를
// 그대로 상속해 쓴다** - 최초 설계는 `kResumeForkedRing3`류 별도
// ring0 프레임 합성이 필요하다고 가정했으나, `Task::init()`이 이미
// 모든 커널 전용 Task(idle/리액터 등)를 위해 정확히 이 일(cs=0x08/
// ss=0x10/rflags=0x202/rip=kTaskStartTrampoline로 채운 TaskTcb 준비)
// 을 하고 있어 그대로 재사용 가능함을 발견했다 - 새 어셈블리/프레임
// 합성 코드 불필요. `isUserLevel`은 항상 false(ring0이므로 CR3
// 재동기화/ring3 진입 로직을 전부 건너뜀), `isKernelMode`는 항상
// true(단순 식별용 플래그 - `kOwnerProcessOf()`가 참고하지는 않는다,
// 어차피 이 클래스는 소유 Process가 없으므로) - `kSpawnKernelThread()`
// 가 채운 뒤 상속받은 `init()`을 그대로 호출한다. v1은 인스턴스당
// 스레드 하나(devmgr/fs 둘 다 현재 단일 스레드).
class KernelThread : public Task, public EnableSharedFromThis<KernelThread> {
public:
    WeakPtr<Task> weakAsTask() { return WeakPtr<Task>(sharedFromThis(), static_cast<Task*>(this)); }
    SharedPtr<KernelThread> sharedSelf() { return sharedFromThis(); }

    // ring0 최초 진입 시 부를 함수 포인터/인자 - 참고용 보관일 뿐(실제
    // 진입 배선은 상속받은 `Task::init(entry, arg)`이 TaskTcb의
    // rbx/r12에 직접 싣는다, 위 클래스 문서 참고).
    void (*entry)(void*) = nullptr;
    void* entryArg = nullptr;

    // UserThread::allocate()/release()/ensureSelfRef()와 완전히 동일한
    // 계약(task.cpp에 구현) - 정적/동적 생성 양쪽 다 `submitterTask`
    // 체이닝이 성립하려면 이 컨트롤 블록이 필요하다.
    static KernelThread* allocate();
    static void release(KernelThread* thread);
    bool ensureSelfRef();

private:
    SharedPtr<KernelThread> _selfRef;
};

// [신규, 2026-09-20, SP-43331889 §2 개정 - 설계자 답변(QU-ECEE5990)
// 반영, 최초 설계였던 `Process::execKernelEntry()`를 대체] devmgr/fs
// 등 "커널 모드 Task"를 Process 없이 곧바로 띄운다 - idle/리액터와
// 동일한 지위(Process 소속 없음). `Process::execImage()`와 동일한
// 관례로 실제 스케줄링(`Scheduler::enqueue()`)은 호출부 책임 - 이
// 함수는 `KernelThread`를 할당/초기화만 하고 반환한다. 실패는
// `KernelThread::allocate()`의 슬랩 고갈 한 가지뿐(ELF 파싱/페이지
// 매핑처럼 실패할 수 있는 단계가 아예 없음)이라 실패 시 nullptr.
KernelThread* kSpawnKernelThread(void (*entry)(void*), void* arg);

// [갱신, 2026-09-20, PN-81E49523 2단계, 설계자 답변(QU-2FC61718)+후속
// 지시("TaskTcb 자체를 Task 구조체에 계속 유지해두고, kContextSwitch에
// 바로 넘길 수 있는 형태로 유지")] 현재 실행 흐름의 완전한 레지스터
// 상태(TaskTcb=InterruptFrame, interrupt_frame.h)를 이 자리에서 직접
// 조립해 저장하고 newTcb로 전환한다 - `*oldTcbSlot`에 전환 전
// TaskTcb*(=`&current->tcb`가 넘기는 그 슬롯)를 기록한다. 복원은
// 예외 없이 `isr_common_epilogue`(isr.S)로 점프해 `newTcb`가 가리키는
// 자리를 그대로 새 RSP로 삼아 진짜 `iretq`로 착지한다(x86_64 표준
// 구현 방식) - 포인터 하나만 넘기면 되므로 별도 복사/변환 오버헤드가
// 없다. 평범한 C++ 함수 호출로 불리는 협조적 전환 전용(Scheduler::
// parkCurrent()/yieldCurrent() 등, 인터럽트에 중첩되지 않음) - 인터럽트
// 핸들러 내부에서는 대신 `kContextSwitchFromISR`을 쓴다.
extern "C" void kContextSwitch(TaskTcb** oldTcbSlot, TaskTcb* newTcb);

// [신규, 2026-09-20, PN-81E49523 2단계, 설계자 지시("컨텍스트 스위치
// 자체를 kContextSwitchFromISR과 kContextSwitch 둘로 나눠 구현")]
// 인터럽트 핸들러 내부(`Scheduler::onTick()`/`onForcedMigration()`,
// 둘 다 하드웨어+`isr_common_stub`이 이미 만들어 둔 진짜
// `InterruptFrame*`을 인자로 받고 있음)에서 전용으로 쓴다 -
// `currentFrame`이 가리키는 그 실제 프레임을 그대로 "이 Task의 마지막
// 캡처"로 삼으므로(다시 조립할 필요가 전혀 없음) `kContextSwitch`보다
// 훨씬 단순하다. 복원 쪽은 `kContextSwitch`와 완전히 동일하게
// `isr_common_epilogue`로 점프 - 두 함수 모두 저장 결과물이 정확히
// 같은 `TaskTcb` 모양이라, 어느 쪽으로 저장됐든 다음 재개는 어느
// 함수를 통해서도 안전하다(`next->tcb`를 그대로 새 `newTcb`로 넘기기만
// 하면 됨). `Scheduler::onTick()`/`onForcedMigration()`이 이 함수
// 하나로 통일되면서, 예전에 있었던 "next가 한 번도 디스패치된 적
// 있는지"(`hasEverRun`)에 따라 `kContextSwitch`/`kContextSwitchToFreshTask`
// 중 고르던 분기 자체가 사라졌다(Task::init()이 짓는 가짜 최초 프레임도
// 이제 완전한 TaskTcb라 이 함수 하나로 균일하게 착지 가능 - PN-414BF822가
// 그 분기를 도입했던 근본 이유(popfq/ret의 RFLAGS 타이밍 위험)가
// iretq 기반 착지로 애초에 사라졌기 때문).
extern "C" void kContextSwitchFromISR(TaskTcb** oldTcbSlot, TaskTcb* newTcb, TaskTcb* currentFrame);

}  // namespace kernel

#endif  // MINICORE_KERNEL_TASK_H
