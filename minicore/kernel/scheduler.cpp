#include "scheduler.h"

#include "acpi.h"
#include "gdt.h"
#include "interrupt_frame.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "panic.h"
#include "process.h"
#include "syscall.h"
#include "syscall_fastpath.h"
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
// 그대로 재사용한다(우선순위 개념이 없는 단순 FIFO면 충분하다).
TaskQueue gCleanupQueues[kMaxCores];

// HPET가 없는 폴백 환경에서 전역 tickCount 공급원 역할을 대신하는
// BSP 코어 인덱스(DC-0CC88ABB/QU-3218B790 설계자 답변 (a), 2026-09-14
// - "BSP 한정으로 Scheduler::onTick이 Timer::onTick()도 대신 호출").
// startTickOnThisCore()의 첫 호출(항상 BSP 자신 - AP는 그 이후
// Smp::startApCores()가 순차 기동)에서 한 번만 확정한다.
uint32_t gBspCoreIndex = 0;
bool gBspCoreIndexKnown = false;

// 부팅 시점(어느 프로세스도 아직 없어 CR3가 여전히 Paging::init()이
// 만든 커널 전용 PML4인 시점)의 CR3 - `Scheduler::init()`에서 한 번만
// 확정한다(PN-63BCFE45 후속 발견, 2026-09-15 실측). **왜 필요한가**:
// `kSyncCr3ForDispatch`가 UserThread로 디스패치할 때는 그 프로세스의
// `userPml4Phys`를 쓰지만, 예전엔 커널 전용 Task(리액터 등)로
// 디스패치할 때는 CR3를 아예 안 건드렸다 - 그러면 그 직전에 실행 중이던
// UserThread의 CR3가 그대로 남는다. 커널 higher-half(direct map/커널
// 이미지)는 모든 프로세스 PML4에 공유돼 있어 대개는 문제가 없지만,
// **부팅 초기 스택(BSP의 kMain()/AP의 kApMain()이 쓰던, 저지대
// identity map 스택 - PN-58501EAA "중요 발견")만은 예외**다 - 이
// 스택은 어느 프로세스의 PML4에도 안 들어있는 lower-half 주소라,
// UserThread의 CR3 아래에서는 접근 자체가 불가능하다. `gIdleSavedRsp`
// (runLoop()이 처음 Task로 전환하기 직전의 자기 자신 RSP)가 정확히
// 이 부팅 스택을 가리키므로, 리액터가 자기 할 일을 마치고 다시
// `parkCurrent()`로 그 idle 컨텍스트로 되돌아가려 할 때(자기 자신의
// 안전한 스택 위에서 실행 중이므로 CR3를 바꿔도 안전하다 - kEnterRing3
// 와 동일한 안전 논리) CR3가 여전히 UserThread의 것으로 남아 있으면
// 그 자리에서 즉시 Double Fault가 난다(실측으로 발견 - `pop r15`가
// 저지대 스택에서 폴트, 그 #GP 전달 자체도 같은 이유로 실패해 #DF로
// 격상). 그래서 커널 전용 Task로 디스패치할 때 이 필드로 CR3를
// 명시적으로 되돌린다.
uint64_t gBootPml4Phys = 0;

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

// 이 코어의 하드웨어 FPU/SSE 레지스터가 지금 어느 Task의 상태를 담고
// 있는지(SP-83A07867 §8, PN-F258698E) - kSyncFpu/Scheduler::handleFpuTrap
// 만 갱신한다. nullptr이면 아직 아무도 이 코어에서 FPU/SSE를 쓴 적이
// 없다는 뜻(부팅 직후 기본값). kSyncCr3의 gBootPml4Phys와 달리 "부팅
// 전용 기본 소유자" 개념이 없다 - idle 컨텍스트 자체는 FPU를 절대 쓰지
// 않으므로 nullptr을 그대로 "소유자 없음"으로 취급해도 충분하다.
Task* gFpuOwner[kMaxCores] = {};

// 선점 비활성화 카운터 - 코어별로 그 코어 자신만 접근한다(인터럽트
// 게이트라 같은 코어 안에서 재진입 없음, 다른 코어는 자기 배열만
// 건드리므로 원자 연산이 필요 없다).
uint32_t gPreemptDisableCount[kMaxCores] = {};

// 이 코어에서 next로 실제로 전환하기(kContextSwitch) 직전마다 부른다
// (PN-AEA74E1B). next가 ring3 코드를 실행할 수 있는 UserThread면(v1은
// isUserLevel==true가 정확히 이 뜻) 이 코어의 TSS.RSP0을 그 Task 자신의
// 커널 스택 top으로 맞춰 둔다 - 안 맞추면 다른 UserThread가 트랩할 때
// 엉뚱한(이전에 디스패치됐던 UserThread의) 커널 스택을 밟는다.
//
// **PN-124C105B("syscall 명령 경로") 추가** - `SyscallFastPath::
// setKernelRspForThisCore()`도 같은 값으로 반드시 같이 갱신해야 한다.
// `syscall` 명령은 TSS.RSP0을 안 쓰고 GS 기반 스크래치를 직접
// 읽으므로(syscall_fastpath.h 참고), 이 두 값이 어긋나면 int 0x80과
// `syscall` 두 경로가 서로 다른 커널 스택을 쓰게 되는 심각한 버그가
// 된다 - 이 함수 하나에서 항상 같이 갱신해 그럴 여지를 없앤다.
//
// **`Scheduler::runLoop()`/`onTick()` 둘 다에서 안전하게 부를 수 있다**
// - TSS/스크래치 구조체에 값을 쓰는 것뿐이라 지금 어떤 스택 위에서
// 실행 중이든(이 함수를 호출하는 시점엔 아직 next의 스택으로 넘어가기
// 전이다) 무해하다. CR3 복원은 이것과 달리 **runLoop()에서는 안전하지
// 않다** - 아래 kSyncCr3ForDispatch 참고.
void kSyncRsp0ForDispatch(Task* next) {
    if (next->isUserLevel) {
        Gdt::setRsp0ForThisCore(next->kernelStackTop);
        SyscallFastPath::setKernelRspForThisCore(next->kernelStackTop);
    }
}

// **`Scheduler::onTick()`에서만 부른다 - `runLoop()`에서 부르면 안 된다**
// (PN-63BCFE45, 실측으로 발견). next가 UserThread면 CR3를 그 Task
// 자신의 유저 주소공간(userPml4Phys)으로 되돌린다 - `kContextSwitch`가
// 저장/복원하는 레지스터 집합(콜리세이브+RFLAGS)에도, `iretq`가
// 복원하는 InterruptFrame에도 CR3는 없다. process.cpp의 kEnterRing3가
// "첫 진입 때만" CR3를 설정하는 것만으로는, 이 Task가 두 번째로
// 디스패치될 때(그 사이 다른 UserThread가 실행되며 CR3를 자기 것으로
// 바꾸어 놓은 뒤) 아무도 CR3를 되돌리지 않아 잘못된 주소공간으로 실행을
// 재개하는 버그가 있었다(서로 다른 프로세스가 우연히 완전히 같은
// 코드/스택 레이아웃이 아닌 한 반드시 크래시 - 코드가 우연히 동일한
// 스레드끼리는 문제없이 도는 것처럼 보여 한동안 발견되지 않았다).
//
// **왜 onTick()에서만 안전한가**: `mov cr3`는 그 자리에서 즉시 전체
// TLB를 무효화하고 이후 모든 메모리 접근을 새 주소공간 기준으로 해석시킨다
// - `onTick()`은 항상 "지금 막 트랩/인터럽트로 끓긴 Task
// 자신의(커널 higher-half, 모든 프로세스가 공유) 스택" 위에서 실행
// 중이므로 안전하다. 반면 `runLoop()`은 idle 상태일 때 코어의 최초
// 부트 스택(BSP의 kMain()/AP의 kApMain()이 쓰던, 저지대 identity map
// 스택 - 어느 프로세스의 PML4에도 안 들어있음, PN-58501EAA "중요
// 발견")에서 실행되고 있을 수 있어, 그 위에서 CR3를 바꾸면 다음
// 스택 접근에서 즉시 폴트/트리플 폴트가 난다 - 그래서 UserThread의
// "첫 진입" CR3 설정은 이 함수가 아니라(runLoop()이 호출하는 자리라)
// kEnterRing3 자신이(이미 그 Task 고유의 안전한 스택으로 넘어온 뒤) 맡는다.
//
// **next가 커널 전용 Task일 때는 `gBootPml4Phys`로 되돌린다**(PN-63BCFE45
// 후속 발견, 2026-09-15 실측) - 예전엔 이 분기가 없어(if만 있고 else
// 없음) UserThread에서 커널 Task(리액터 등)로 전환할 때 CR3가 직전
// UserThread의 것으로 계속 남았다. 커널 higher-half는 공유돼 있어
// 대개는 무해하지만, `gBootPml4Phys` 문서 주석이 설명하는 부팅 스택
// (`gIdleSavedRsp`가 가리키는 곳)만은 예외라 실제로 Double Fault를
// 유발했다(리액터가 자기 일을 마치고 parkCurrent()로 idle 컨텍스트에
// 되돌아가려는 순간 실측 발견).
//
// **통합 및 최적화(SP-83A07867, QU-892AB38A 설계자 답변, 2026-09-15)**:
// 이 함수(기존 이름 kSyncCr3ForDispatch)가 onTick()에서만 안전하다는
// 제약 자체는 그대로다(위 문서 주석 참고 - PN-58501EAA의 부팅 스택
// 안전성 논리는 변하지 않았다) - 달라진 건 두 가지뿐이다: (1) 이름을
// `kSyncCr3`로 통일해 kTaskStartTrampoline(아래 kSyncCr3OnTaskStart
// 참고)/yieldCurrent()/parkCurrent() 재개 지점과 정확히 같은 로직을
// 공유하게 했고(전엔 CR3 동기화가 이 함수 하나에만 있어 나머지
// 세 지점은 아예 손대지 않았다 - SP-83A07867 §2가 정리한 근본 원인),
// (2) `Paging::currentPml4Phys()`로 현재 값을 먼저 읽어 target과
// 같으면 `mov cr3` 자체를 생략하는 최적화를 추가했다(§5 - 불필요한
// 전체 TLB flush 회피, 레지스터 읽기 자체는 매우 저렴해 항상 이득).
void kSyncCr3(Task* task) {
    const uint64_t targetPml4 = task->isUserLevel ? task->userPml4Phys : gBootPml4Phys;
    if (Paging::currentPml4Phys() != targetPml4) {
        asm volatile("mov %0, %%cr3" : : "r"(targetPml4) : "memory");
    }
}

// kSyncCr3와 정확히 같은 세 지점(§3.2 갈래①/②)에서 같은 이유로 호출된다
// (SP-83A07867 §8 - "FPU 상태 관리는 별도의 새 디스패치 훅을 파지 않고
// §3.2의 공용 진입점에 CR0.TS 제어 로직을 삽입하는 방식으로 구현할
// 것"). kSyncCr3와 달리 실제로 레지스터 내용을 옮기지 않는다(FXSAVE/
// FXRSTOR는 비싸므로 여기서 미리 하지 않고, #NM 트랩이 실제로 필요한
// 순간에만 하도록 미룬다 - lazy 전략의 핵심) - 이 함수가 하는 일은
// 오직 "이 Task가 이미 이 코어 하드웨어의 현재 소유자인가"만 보고
// CR0.TS를 세우거나(다르면, 다음 FPU/SSE 명령에서 #NM 유도) 지우는
// 것뿐이다(같으면, 트랩 없이 바로 쓰게 허용 - 예를 들어 짧은 시간 안에
// 같은 Task가 반복 디스패치되는 경우 불필요한 트랩 반복을 피한다).
void kSyncFpu(Task* task, uint32_t coreIndex) {
    if (gFpuOwner[coreIndex] == task) {
        asm volatile("clts");
        return;
    }
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %0, %%cr0" : : "r"(cr0 | (1ULL << 3)) : "memory");
}

// kSyscallEndpointSelfTerminate(PN-71C3D483)의 실제 핸들러 - args는
// kTaskOnFallingToEnd가 `Syscall::submitDetached()`로 넘긴, 종료 대상
// UserThread 자신(Task*)이다. **이 onExec이 실행되고 있다는 사실 자체가
// target이 이미 이 코어에서 실행 중이 아님을 증명한다**(한 코어에서는
// 항상 하나의 Task만 실행되고, 지금은 리액터가 실행 중이므로) -
// target은 kTaskOnFallingToEnd에서 이미 스스로를 Zombie로 표시해 둠
// (그래서 `Scheduler::onTick()`이 그 이후로 다시는 재삽입하지 않았다),
// Scheduler::retireTask() 문서 주석 참고.
//
// **항목 3(Process 자원 회수, PN-71C3D483) 완료** - 커널 스택 회수
// (retireTask)만으로는 이 UserThread가 쓰던 유저 주소공간이 그대로
// 남는다. self-terminate는 항상 isUserLevel Task에서만 제출되므로
// (kTaskOnFallingToEnd의 분기 참고) target을 UserThread로 안전하게
// 캐스팅할 수 있다 - v1은 프로세스당 스레드 하나뿐이라(process.h
// 클래스 문서) 이 스레드가 끝나는 순간이 곧 그 Process 전체가 끝나는
// 순간과 같다. `process` 필드는 execImage()가 항상 채워 두지만
// (process.cpp의 `thread->process = this;`) 방어적으로 null 확인한다.
class SelfTerminateHandler : public AsyncTaskHandler {
public:
    void onExec(AsyncTask*, void* args) override {
        auto* target = static_cast<Task*>(args);
        Scheduler::retireTask(target);
        auto* userThread = static_cast<UserThread*>(target);

        // [PN-40E976F2] 이 UserThread가 제출했지만(Syscall::submit)
        // 아직 wait()로 소비되지 않은 AsyncTask들을 정리한다 - 이제
        // 그 결과를 가져갈 사람이 영원히 없다. pendingSyscalls 자체가
        // 설계자가 말한 "사망 전파 목록"이다(이미 있는 자료구조를
        // 그대로 재사용 - ChunkedList::clear() 문서 주석이 애초에 이
        // 용도를 예정해 뒀다).
        userThread->pendingSyscalls.forEach([](UserThread::PendingSyscall& pending, auto*) {
            auto* asyncTask = reinterpret_cast<AsyncTask*>(pending.token);
            if (asyncTask->state == AsyncTaskState::Completed || asyncTask->state == AsyncTaskState::Failed) {
                // 이미 끝났지만 아무도 wait()로 가져가지 않은 결과 -
                // 리액터는 autoFree=false라 이미 손을 뗀 상태이므로
                // 여기서 대신 반납한다.
                GenericSlabAllocator::free(reinterpret_cast<void*>(asyncTask->stackBase), kAsyncTaskStackSize);
                GenericSlabAllocator::free(asyncTask, sizeof(AsyncTask));
                return;
            }
            // 아직 안 끝났다 - 취소로 전이한다. 이제 아무도 결과를
            // 가져가지 않으므로 autoFree를 강제로 켜서 리액터가 스스로
            // 반납하게 한다.
            const bool wasSuspended = (asyncTask->state == AsyncTaskState::Suspended);
            asyncTask->autoFree = true;
            asyncTask->state = AsyncTaskState::Cancelled;
            if (wasSuspended) {
                // Ready(이미 실행 큐에 있음)라면 언젠가 popFront될 때
                // 자연히 Cancelled를 발견한다 - 하지만 Suspended(스스로
                // yield하고 큐 밖으로 나가 있는 상태)라면 아무도 다시
                // 큐에 넣어주지 않는 한 영원히 방치된다. 이 함수 자신이
                // 그 AsyncTask를 원래 실행했던 바로 그 코어의 리액터
                // 위에서 돌고 있으므로(v1 - 코어 간 이관 없음)
                // submitCompletion을 직접 불러도 안전하다.
                AsyncReactor::submitCompletion(asyncTask);
            }
        });
        userThread->pendingSyscalls.clear();

        if (userThread->process) {
            // Resurrect(SP-EAB162FC §6) - destroy() 이후에도 Process
            // 객체 자체(캐스팅 근거: 정적/장기수명 인스턴스 - destroy()는
            // 주소공간만 반납할 뿐 이 구조체를 지우지 않는다)는 살아있어
            // startFlags/resurrectCount를 안전하게 읽을 수 있다. 재스폰은
            // 기존 주소공간이 완전히 반납된 뒤에 한다(자원 회수 -> 재생성
            // 순서).
            Process* process = userThread->process;
            const ProcessStartFlags startFlags = process->startFlags;
            const uint32_t newResurrectCount = process->resurrectCount + 1;
            process->destroy();
            if (startFlags.resurrect && startFlags.respawn) {
                // §6.4 크래시 루프 방지 - 연속 kMaxResurrectAttempts회에
                // 도달하면 재스폰을 아예 시도하지 않고 커널 전체를
                // 멈춘다("커널 서비스는 커널을 대행하는 존재라 반복
                // 재크래시는 개별 프로세스 문제가 아니라 커널 자체가
                // 정상 동작할 수 없는 상태" - 설계자 지시, silent
                // degraded mode 금지).
                if (newResurrectCount >= Process::kMaxResurrectAttempts) {
                    kPanic("KernelService resurrect limit exceeded");
                }
                startFlags.respawn(newResurrectCount);
            }
        }
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

SelfTerminateHandler gSelfTerminateHandler;

}  // namespace

void Scheduler::init() {
    gCoreCount = Acpi::cpuCount();
    if (gCoreCount == 0) {
        gCoreCount = 1;
    }
    if (gCoreCount > kMaxCores) {
        gCoreCount = kMaxCores;
    }
    // 이 시점은 아직 어느 프로세스도 없어(kSpawnInitProcess()는 이보다
    // 한참 뒤) CR3가 여전히 Paging::init()이 만든 커널 전용 PML4다 -
    // gBootPml4Phys 문서 주석 참고. BSP에서 한 번만 호출된다(이 함수
    // 자체가 kmain.cpp에서 한 번만 불림).
    gBootPml4Phys = Paging::currentPml4Phys();
    // BSP에서 한 번만(다른 registerSyscallEndpoints류 호출과 같은 이유
    // - 이미 쓰인 슬롯에 재등록하면 SyscallRegistry::registerHandler가
    // 거부한다) - kSyscallEndpointSelfTerminate(값 0)는 syscall.h가
    // 예약해 둔 고정 슬롯(PN-71C3D483 완료 전까지는 핸들러 없이
    // 비어 있었다).
    SyscallRegistry::registerHandler(kSyscallEndpointSelfTerminate, &gSelfTerminateHandler);
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
    // **실측으로 발견한 경쟁의 구조적 방지책(2026-09-14, Channel IPC
    // 스트레스 테스트)**: "이 Task를 어디선가에서 이미 큐에 넣어 둔
    // 시점"과 "그걸 아직 모르는 다른 호출부가 별도로 또
    // enqueue/scheduleImmediate를 부르는 시점" 사이의 창은
    // (reactorTaskEntry의 parkCurrent() 진입 직전, Syscall::wait()의
    // parkCurrent() 진입 직전 등 - 이번 세션에 개별적으로 찾아 cli로
    // 막은 지점들 참고)은 원리상 스케줄러 틱이 "이 Task는 아직 안
    // 자고 있으니 그냥 라운드로빈으로 넘어간다"고 판단할 수 있는 모든
    // 지점에서 잠재적으로 생길 수 있어 하나하나 찾아 막는 방식만으로는
    // 끝이 없다 - 이미 어느 큐에 들어 있는 Task를 다시 넣지 않는
    // 것으로 근본 클래스 자체를 막는다. **state(Ready/Running/...)는
    // 이 용도로 사용할 수 없다** - Task::init() 직후에도 이미 state=Ready라
    // "아직 한 번도 큐에 들어간 적 없음"과 "이미 큐에 있음"을 구분하지
    // 못한다 - 그래서 별도의 Task::inRunQueue 플래그를 사용한다. cli로
    // "확인 + 세팅 + push"를 통째로 원자적으로 묶어야 확인 자체가
    // 틱과 경쟁하지 않는다.
    asm volatile("cli");
    if (task->inRunQueue) {
        asm volatile("sti");
        return;  // 이미 어느 큐에 들어 있다 - 다시 넣으면 이중 스케줄링
    }
    task->inRunQueue = true;
    task->state = TaskState::Ready;
    if (task->taskClass == TaskClass::RealTime) {
        gRtQueues[coreIndex].pushBack(task);
    } else {
        gNormalQueues[coreIndex].pushBack(task);
    }
    asm volatile("sti");
}

void Scheduler::scheduleImmediate(uint32_t coreIndex, Task* task) {
    if (coreIndex >= gCoreCount) {
        coreIndex = 0;
    }
    // enqueue()와 같은 이유 - 위 주석 참고.
    asm volatile("cli");
    if (task->inRunQueue) {
        asm volatile("sti");
        return;
    }
    task->inRunQueue = true;
    task->state = TaskState::Ready;
    gImmediateQueues[coreIndex].pushBack(task);
    asm volatile("sti");
}

Task* Scheduler::pickNext(uint32_t coreIndex) {
    if (coreIndex >= gCoreCount) {
        coreIndex = 0;
    }
    Task* task = gImmediateQueues[coreIndex].popFront();
    if (!task) {
        task = gRtQueues[coreIndex].popFront();
    }
    if (!task) {
        task = gNormalQueues[coreIndex].popFront();
    }
    if (task) {
        // 큐에서 실제로 빠져나온 순간 inRunQueue를 내려야 한다 -
        // enqueue()/scheduleImmediate()의 "이미 큐에 있으면 재삽입
        // 생략" 판단이 이 시점부터는 다시 "새로 넣어도 됨"으로
        // 정확히 반영되게 한다.
        task->inRunQueue = false;
    }
    return task;
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
    // Timer::init()도 더 이상 이 하드웨어를 재프로그래밍하지 않는다
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
    // 별도로 자신의 주기 인터럽트를 프로그래밍하면 이 스케줄러 틱
    // 자체를 덮어써 버린다(실측 전 리뷰로 확인). 그래서 HPET가 없을
    // 때는 BSP 코어의 이 스케줄러 틱이 전역 시각도 대신 공급한다 -
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
        return;  // 대기 중인 다른 Task 없음 - 그대로 계속 실행(타임퀵텀 소진 안 함)
    }

    // Zombie면(PN-71C3D483 - kTaskOnFallingToEnd가 self-terminate 제출
    // 직전 스스로 표시해 둔 상태) 재삽입하지 않는다 - 곧 리액터의
    // SelfTerminateHandler가 Scheduler::retireTask(current)로 정리
    // 큐에 등록한다. 그 전까지 이 Task는 어느 큐에도 없는 채로 그냥
    // "스위칭되어 나간" 상태로만 남는다(다시 뽑힐 걱정 없음).
    if (current->state != TaskState::Zombie) {
        enqueue(coreIndex, current);  // 라운드로빈 - Ready로 큐 꼬리에 재삽입
    }
    gCurrentTask[coreIndex] = next;
    next->state = TaskState::Running;
    kSyncRsp0ForDispatch(next);
    kSyncCr3(next);
    kSyncFpu(next, coreIndex);
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
        // next 자신의 것처럼 다시 가로채어 버린다(next->savedRsp가
        // idle 스택의 스냅샷으로 덮어쓰임 - 실측으로 발견한 버그).
        // 여기서 끝 인터럽트는 kContextSwitch의 pushfq/popfq를 통해
        // idle 쪽에만 저장되고(나중에 idle이 재개될 때만 다시 반영),
        // next는 자신이 마지막으로 저장해 둔 RFLAGS(보통 IF=1)로
        // 독립적으로 재개되므로 next 쪽으로 "인터럽트 꺼짐"이 새어
        // 나가지 않는다.
        asm volatile("cli");
        gCurrentTask[coreIndex] = next;
        next->state = TaskState::Running;
        // CR3는 여기서 안 건드린다(kSyncCr3 문서 주석 참고 - 이 idle
        // 컨텍스트의 스택이 안전하지 않을 수 있다). **SP-83A07867로
        // 더 이상 여기서 신경 쓸 필요가 없다** - 이 kContextSwitch가
        // 도착하는 지점(최초 실행이면 kTaskStartTrampoline의
        // kSyncCr3OnTaskStart 호출, yieldCurrent/parkCurrent로
        // 파킹됐다가 재개되는 것이면 그 함수들 자신의 재개 지점)이
        // 전부 자기 자신의 안전한 스택으로 이미 넘어온 뒤 CR3를
        // 동기화하므로, runLoop()은 그 도착 지점이 무엇이든 몰라도
        // 된다(§3.2 - 이 설계의 핵심 이점).
        kSyncRsp0ForDispatch(next);
        kContextSwitch(&gIdleSavedRsp[coreIndex], next->savedRsp);
        // yieldCurrent()로 되돌아온 경우에만 이 지점으로 온다(onTick의
        // Task-to-Task 직접 전환은 이 프레임을 거치지 않는다) - 다음
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
    // 종류 경쟁과 동일한 이유로 cli를 사용한다.
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
    // **SP-83A07867(QU-892AB38A 설계자 답변, 2026-09-15) - 이 재개
    // 지점이 바로 §3.2 갈래②의 세 곳 중 하나다.** 위 kContextSwitch가
    // 반환한 이 시점은 이미 이 Task 자신의(안전한) 스택으로 넘어온
    // 뒤라 CR3를 동기화해도 된다 - 파킹되기 전 다른 UserThread가 실행
    // 되며 CR3를 자기 것으로 바꾸어 놓았을 수 있는데, 예전엔 이 경로가
    // 전혀 CR3를 건드리지 않아 "아직 실제로 발현되지 않은 세 번째
    // 공백"으로 남아 있었다(지금은 이 프로젝트의 어떤 ring3 코드도
    // yieldCurrent를 타지 않아 관찰되지 않았을 뿐이다).
    kSyncCr3(current);
    kSyncFpu(current, coreIndex);
    // **실측으로 발견한 버그(2026-09-14, Channel IPC 스트레스
    // 테스트)**: 위 kContextSwitch의 pushfq는 방금 실행한 cli 때문에
    // IF=0인 RFLAGS를 이 Task 자신의 저장 슬롯에 그대로 담아 버린다 -
    // 이 재개 지점은 인터럽트 프레임을 거치는 iretq가 아니라 순수
    // 스택 포인터 교환(popfq)이라, "인터럽트가 꺼진 채로 저장했다가
    // 그대로 복원"이 반복될 뿐 저절로 IF=1로 돌아오지 않는다 - 이
    // Task가 yieldCurrent()/parkCurrent()를 단 한 번이라도 거치고 나면
    // 그 뒤로는 매번 IF=0으로 재개되고, runLoop()이 "정말 대기할
    // 때"(sti;hlt)에 도달하기 전까지는 이 코어의 인터럽트(스케줄러
    // 틱 포함)이 아예 걸리지 않게 된다 - 부하가 계속 이어져 그
    // hlt 분기에 도달하지 못하면 사실상 영구히 멈춘다(Channel IPC처럼
    // 여러 Task가 끊임없이 서로를 깨우는 워크로드에서 실측 발견).
    // 그래서 재개 직후 여기서 명시적으로 다시 켠다 - 정상적으로
    // 실행 중인 Task는 항상 IF=1이어야 한다는 불변조건을 저장된 값에
    // 기대지 않고 직접 강제한다.
    asm volatile("sti");
}

void Scheduler::parkCurrent() {
    // yieldCurrent()와 같은 이유로 cli - gCurrentTask를 지우기 전에
    // 상태만 Blocked로 바꾸면, 그 사이 끼인 스케줄러 틱이 이 Task를
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
    // SP-83A07867 §3.2 갈래②의 나머지 한 곳 - yieldCurrent()의 재개
    // 지점과 완전히 동일한 이유로 여기서도 CR3를 동기화한다(위
    // yieldCurrent() 주석 참고 - 이 함수가 첫 실제 소비자가 되기
    // 전까지는 아직 발현되지 않았던 공백이었다).
    kSyncCr3(current);
    kSyncFpu(current, coreIndex);
    // 누군가 깨워 runLoop이 이 Task를 다시 고를 때까지 여기서 멈춰
    // 있다가, 다시 선택되면 이 지점부터 재개된다 - yieldCurrent()와
    // 같은 이유로(위 주석 참고) 여기서도 명시적으로 다시 켜야 한다 -
    // 저장된 RFLAGS에 기대면 cli 때문에 IF=0인 채로 복원되어, 이
    // Task가 다시 파킹되기 전까지 이 코어의 인터럽트가 전부 막힌다.
    asm volatile("sti");
}

void Scheduler::retireCurrentTask() {
    // yieldCurrent()/parkCurrent()와 같은 이유로 cli - gCurrentTask를
    // 지우기 전에 clean-up 큐에 먼저 넣으면, 그 사이 끼인 스케줄러 틱이
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

void Scheduler::retireTask(Task* task) {
    // scheduler.h의 문서 주석 참고 - 호출자 자신이 지금 이 코어에서
    // 실행 중이라는 사실 자체가 target은 이미 실행 중이 아님을
    // 보장한다(한 코어 = 동시에 하나의 Task). retireCurrentTask()와
    // 달리 kContextSwitch가 필요 없다 - target은 스위칭할 "실행 중인
    // 자기 자신"이 아니라 이미 정지해 있는 다른 Task이므로, **커널
    // 스택을 여기서 바로 회수한다**(지연 큐 없음 - PN-645CF608
    // Resurrect 도입으로 지연 회수가 use-after-reuse 위험이 됨,
    // scheduler.h 문서 주석 참고).
    task->state = TaskState::Zombie;
    PageFrameAllocator::freeOrder(task->kernelStackPhys, kOrderForCleanup(task->kernelStackSize));
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

// idt.cpp의 kIsrHandler가 벡터 7(#NM)마다 호출한다(SP-83A07867 §8,
// PN-F258698E) - kSyncFpu가 디스패치마다 CR0.TS를 세워 둔다가, 이
// Task가 실제로 FPU/SSE 명령을 처음 실행하는 순간에만 하드웨어가 이
// 트랩을 건다. **CLTS를 가장 먼저 한다** - 이 핸들러 자신도, 재개된
// 원래 명령도 더 이상 트랩 없이 FPU/SSE를 사용할 수 있어야 하기 때문이다
// (FXSAVE/FXRSTOR 자체도 TS=1이면 마찬가지로 #NM을 유발한다).
void Scheduler::handleFpuTrap() {
    asm volatile("clts");
    const uint32_t coreIndex = currentCoreIndex();
    Task* current = gCurrentTask[coreIndex];
    if (!current) {
        return;  // idle 컨텍스트는 FPU/SSE를 사용하지 않는다 - 이론상 도달 불가
    }
    Task* owner = gFpuOwner[coreIndex];
    if (owner == current) {
        return;  // 이미 이 Task가 소유자인데 걸린 가짜 트랩(kSyncFpu가 놓친 경우 없음) - 방어적 처리
    }
    if (owner) {
        asm volatile("fxsave (%0)" : : "r"(owner->fpuState) : "memory");
    }
    if (current->fpuInitialized) {
        asm volatile("fxrstor (%0)" : : "r"(current->fpuState) : "memory");
    } else {
        // 이 Task가 FPU/SSE를 사용하는 게 처음이다 - 이전 소유자가 남긴
        // 낡은 상태를 물려받지 않도록 깨끗한 초기 상태로 시작한다.
        asm volatile("fninit");
        current->fpuInitialized = true;
    }
    gFpuOwner[coreIndex] = current;
}

}  // namespace kernel

// context_switch.S의 kTaskStartTrampoline이 entry 콜백(`call rbx`)을
// 부르기 직전에 호출한다 - SP-83A07867 §3.2 갈래②의 세 지점 중
// "Task가 태어나서 처음 실행되는 지점". 이미 이 Task 자신의(이제 막
// kContextSwitch로 넘어온) 스택 위에서 실행 중이라 CR3를 바꿔도
// 안전하다(kEnterRing3가 예전엔 UserThread 한정으로 직접 하던 일 -
// 이제 모든 Task의 첫 실행에 동일하게 적용된다, kEnterRing3 자신의
// 수동 CR3 설정은 이 함수로 대체되어 제거됨). r12(entry arg)/
// rbx(entry 함수 포인터)는 System V 콜리세이브라 이 호출 전후로
// 그대로 보존된다 - 어셈블리 쪽에서 별도로 save/restore할 필요 없음.
extern "C" void kSyncCr3OnTaskStart() {
    kernel::Task* self = kernel::Scheduler::currentTask();
    if (self) {
        kernel::kSyncCr3(self);
        // SP-83A07867 §8/PN-F258698E - "Task가 태어나서 처음 실행되는
        // 지점"도 §3.2 갈래②의 세 곳 중 하나라 kSyncFpu를 그대로 같이
        // 부른다(이름은 kSyncCr3OnTaskStart로 남겨 둔다 - context_switch.S가
        // 이 심볼명을 직접 참조하고, 이 함수 자체가 "디스패치 도착 지점
        // 하나"라는 §3.2의 단위이지 CR3 전용 훅이 아니었다는 것이 원래
        // 설계 의도였으므로 새 심볼을 만들지 않는다).
        kernel::kSyncFpu(self, kernel::Scheduler::currentCoreIndex());
    }
}

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
        // User-Level로 격하된 Task(설계자 지시 1번, PN-71C3D483로
        // 실제 정리 경로까지 완성됨) - **반드시 제출 전에** 스스로를
        // Zombie로 표시해야 한다(QU-84E5B3D5 - Scheduler::retireTask()
        // 문서 주석 참고) - 그래야 잠시 뒤 이 Task가 스위칭되어 나갈
        // 때 `Scheduler::onTick()`이 라운드로빈 재삽입을 건너뛰어, 다시는
        // 이 Task가 pickNext에 뽑히지 않는다는 보장이 성립한다.
        // 자기종료 syscall은 wait 없이 제출만 하고(Syscall::
        // submitDetached - autoFree라 결과를 아무도 안 봐도 리액터가
        // 알아서 정리한다) 반환한다 - 리액터가 나중에 비동기적으로
        // SelfTerminateHandler::onExec에서 Scheduler::retireTask(self)
        // 로 실제 정리(커널 스택 즉시 회수 - PN-645CF608부터는 지연
        // 큐 없음, scheduler.h 문서 주석 참고)를 수행한다. 그 사이
        // (제출 후 ~ 리액터가 실제로 처리하기 전) 이 Task는 그냥 hlt
        // 루프에서 계속 대기한다.
        self->state = kernel::TaskState::Zombie;
        kernel::Syscall::submitDetached(kernel::kSyscallEndpointSelfTerminate, self);
        return;
    }
    // Kernel-Level Task가 계속 커널에 머물러 있는 경우(설계자 지시
    // 2번, 지금 이 프로젝트의 모든 Task가 해당) - 절대 돌아오지 않는다.
    kernel::Scheduler::retireCurrentTask();
}
