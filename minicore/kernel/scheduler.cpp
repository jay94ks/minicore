#include "scheduler.h"

#include "acpi.h"
#include "async_task.h"
#include "delayed_exec.h"
#include "gdt.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "lapic.h"
#include "libkcont/intrusive_list.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"
#include "nmi.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "panic.h"
#include "debug_session.h"
#include "process.h"
#include "rcu.h"
#include "resource_group.h"
#include "serial.h"
#include "smp.h"
#include "syscall.h"
#include "syscall_fastpath.h"
#include "timer.h"
#include "wait_queue.h"

namespace kernel {

void TaskQueue::pushBack(Task* task) {
    {
        SpinlockGuard guard(_lock);
        task->next.store(nullptr);
        if (_tail) {
            _tail->next.store(task);
        } else {
            _head = task;
        }
        _tail = task;
    }
    // 임계구역 밖에서 원자적으로 갱신(SP-9525C4C0 §2.1) - 근사치라
    // 정밀 동기화 불필요.
    _approxLength.fetchAdd(1);
}

void TaskQueue::pushFront(Task* task) {
    {
        SpinlockGuard guard(_lock);
        task->next.store(_head);
        _head = task;
        if (!_tail) {
            _tail = task;
        }
    }
    _approxLength.fetchAdd(1);
}

Task* TaskQueue::popFront() {
    Task* task;
    {
        SpinlockGuard guard(_lock);
        task = _head;
        if (task) {
            _head = task->next.load();
            if (!_head) {
                _tail = nullptr;
            }
            task->next.store(nullptr);
        }
    }
    if (task) {
        _approxLength.fetchSub(1);
    }
    return task;
}

bool TaskQueue::isEmpty() const {
    return _head == nullptr;
}

uint32_t TaskQueue::approxLength() const {
    return _approxLength.load();
}

namespace {

constexpr uint32_t kMaxCores = kAcpiMaxCpus;

// Push/Pull 로드밸런싱(PN-04D6197A, SP-9525C4C0 §4) - idle(hlt) 상태일
// 수 있는 코어를 즉시 깨우는 전용 IPI 벡터. RM-28225668에 이미 배정된
// 값(0xE1) 그대로 - tlb_shootdown.cpp/async_task.cpp와 동일한 관례로
// ISR은 EOI조차 직접 보내지 않는다(idt.cpp의 kIsrHandler가 등록된
// 동적 핸들러 호출 후 대신 보낸다) - 벡터가 뭘 나르는지는 중요하지
// 않다, hlt가 어떤 인터럽트로도 깨어나기만 하면 된다.
constexpr uint32_t kLoadBalanceWakeVector = 0xE1;

// 3단 우선순위(6단계 RT 클래스 + 8-1단계 즉시 스케줄링) - pickNext가
// 이 순서(immediate -> rt -> normal)로 훑는다. 셋을 하나로 합치지
// 않은 이유: "즉시 스케줄링은 RT보다도 먼저"를 큐 자체의 우선순위로
// 표현하면 pushFront 같은 순서 트릭 없이 자명해진다.
TaskQueue gImmediateQueues[kMaxCores];
TaskQueue gRtQueues[kMaxCores];

// [신규, 2026-09-17, SP-B26CDBDD §3.1, PN-158B6B2F] `gNormalQueues`의
// 정렬 키 - vruntime 오름차순(작을수록 "덜 받았다" -> 먼저 뽑힘).
struct TaskVruntimeTraits {
    using Key = uint64_t;
    static Key keyOf(const Task& t) { return t.vruntime; }
    static constexpr Node Task::* Link = &Task::vruntimeLink;
};

// [신규, 2026-09-17, SP-B26CDBDD §3.1] `libkcont::OrderedList<Task,
// TaskVruntimeTraits>` 위에 `TaskQueue`(위)가 이미 갖고 있던 두 가지를
// 그대로 얹은 얇은 래퍼 - **자체 Spinlock**(Pull이 "다른 코어"의 이
// 큐를 직접 훔쳐가므로 그 코어 자신의 `cli`만으로는 보호되지 않는다.
// `OrderedList` 자신은 침습적 컨테이너일 뿐 동시성 보호를 전혀
// 제공하지 않는다는 게 libkcont의 명시적 계약 - `TaskQueue`가 원래
// 이 이유로 `Spinlock`을 갖고 있었던 것과 정확히 동일한 근거라, 이
// 교체가 기존에 이미 성립해 있던 "코어 간 큐 접근은 항상 락으로
// 보호된다"는 불변조건을 조용히 깨지 않도록 그대로 이어받는다 -
// SP-B26CDBDD 자신은 이 점을 명시하지 않았지만, 새 설계 결정이
// 아니라 기존 안전성 보장을 유지하기 위한 구현 세부로 판단해 별도
// 확인 없이 추가했다) + `approxLength()`(TaskQueue와 동일한 근사치
// 원자 카운터 관례, Push/Pull 임계치 판정이 그대로 재사용).
class NormalQueue {
public:
    void init() { _list.init(); }

    void insert(Task* task) {
        SpinlockGuard guard(_lock);
        _list.insert(task);
        _approxLength.fetchAdd(1);
    }

    // §2.3 굶주림 방지 보정용 - 삽입 없이 현재 최솟값 vruntime만 읽는다.
    // 비어 있으면 true(호출부가 보정을 건너뛰게).
    bool minVruntime(uint64_t* outValue) const {
        SpinlockGuard guard(_lock);
        Task* task = _list.first();
        if (!task) {
            return false;
        }
        *outValue = task->vruntime;
        return true;
    }

    // 최솟값(vruntime)을 큐에서 제거하며 반환 - 비어 있으면 nullptr.
    Task* popMin() {
        SpinlockGuard guard(_lock);
        Task* task = _list.first();
        if (task) {
            OrderedList<Task, TaskVruntimeTraits>::remove(task);
            _approxLength.fetchSub(1);
        }
        return task;
    }

    uint32_t approxLength() const { return _approxLength.load(); }

private:
    mutable Spinlock _lock;
    OrderedList<Task, TaskVruntimeTraits> _list;
    AtomicU32 _approxLength;
};

NormalQueue gNormalQueues[kMaxCores];
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

// [PN-F443FE73, SP-677210E6 "워치독(Watchdog)"] 코어마다 자기 틱
// 핸들러가 100Hz로 증가시키는 하트비트 - 최근에 늘지 않았다는 것은
// "이 코어가 최근 1초 가까이 자기 타이머 인터럽트조차 처리 못 했다"
// 는 뜻이다(cli를 오래 쥐고 있거나 그 외 이유로 멈춘 경우 - 일반
// Fixed IPI는 그런 코어에 전달되지 않지만 NMI는 마스크 불가라 여기서
// 진단 가치가 있다). BSP(코어0) 자신의 틱 핸들러가 감시자 역할을
// 겸한다(전용 워치독 코어를 따로 두지 않음 - 단순함 우선).
AtomicU32 gHeartbeat[kMaxCores];
uint32_t gHeartbeatLastSeen[kMaxCores] = {};
bool gWatchdogTriggered[kMaxCores] = {};
uint32_t gWatchdogTickCounter = 0;
// 1초(100Hz 틱 * 100) - 설계자 확인이 필요한 지점이 아닌 구현 세부
// (SP-677210E6 "1초 임계값/체크 주기는 구현 세부" 참고, 실측 후 조정
// 가능).
constexpr uint32_t kWatchdogCheckIntervalTicks = 100;

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
// UserThread의 CR3 아래에서는 접근 자체가 불가능하다. 이 문단 작성
// 당시엔 `gIdleSavedRsp`(runLoop()이 처음 Task로 전환하기 직전의
// 자기 자신 RSP, 2026-09-19 PN-D47FBB8D로 `gIdleTask[coreIndex].
// savedRsp`에 흡수됨)가 정확히 이 부팅 스택을 가리켰으므로, 리액터가
// 자기 할 일을 마치고 다시
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

// [갱신, 2026-09-19, PN-D47FBB8D] 예전엔 이 배열(RSP 하나만 저장)이
// "idle 컨텍스트"의 유일한 표현이었으나, `gCurrentTask[coreIndex]`가
// idle 상태를 그냥 `nullptr`로만 나타내던 탓에 `Scheduler::pickNext()`
// 가 idle을 반환 가능한 정식 후보로 다룰 수 없었다 - `onTick()`의
// `next == nullptr` 분기가 freeze/디버그 정지 검사(`kCheckAndMarkFrozen`/
// `kIsPausedByDebugger`)를 건너뛰는 실측 버그(PN-D47FBB8D)의 근본
// 원인이었다(SP-F682B889 §3.4 재정정 참고). 이제 코어당 정확히 하나인
// 진짜 `Task` 인스턴스(`gIdleTask`, 아래)로 승격했다 - 이 배열은
// `gIdleTask[coreIndex].savedRsp`로 흡수돼 더 이상 필요 없다(제거됨).

// [신규, 2026-09-19, PN-D47FBB8D] 코어당 정확히 하나 존재하는 idle/
// 리액터 통합 Task - `TaskClass::Idle`(task.h 문서 주석 참고),
// `Scheduler::enterIdleLoop()`이 부팅 극초반 딱 한 번 초기화하고,
// 그 뒤로는 `Scheduler::pickNext()`가 세 큐 모두 비었을 때
// `onTick()`/`onForcedMigration()`이 대체 후보로 쓰는 유일한 통로다
// (pickNext() 자신은 이 Task를 절대 반환하지 않는다 - 세 큐 중
// 어디에도 안 들어가므로 애초에 뽑힐 수 없다, 호출부가 명시적으로
// 폴백해야 함). **일반 `Task::init()`을 쓰지 않는다** - 부팅 극초반
// (Page/Slab 할당자가 아직 준비 안 됐을 수 있는 시점, 아래 gIdleStack
// 문서 주석과 같은 이유)에 구성돼야 해서 `enterIdleLoop()`이 이
// 구조체의 필드(savedRsp/state/hasEverRun/taskClass)를 직접 채운다 -
// `kernelStackPhys`/`kernelStackSize`/`kernelFsBase`는 의도적으로
// 비워 둔다(이 Task는 절대 스스로 종료(cleanup 큐)되거나 ring3/TLS를
// 쓰지 않으므로 무해 - kSyncRsp0ForDispatch/kSyncFsBase 둘 다
// isUserLevel==false면 아무 것도 안 함).
Task gIdleTask[kMaxCores];

// [신규, 2026-09-17, PN-2008220B] 코어별 idle 컨텍스트 전용 스택 -
// `enterIdleLoop()`이 부팅 극초반(BSP의 kMain()/AP의 kApMain()이
// 아직 저지대 identity map 스택 위에 있는 시점)에 이 스택으로 딱
// 한 번 옮겨 앉은 뒤로는 이 코어의 idle 컨텍스트가 영구히 여기서만
// 산다. 정적 배열이라 커널 이미지/BSS 안에 위치해(gCallSlotPool과
// 같은 이유로 슬랩/페이지 할당자 초기화 순서에 의존하지 않는다,
// SP-E9B44929 §6-A가 겪은 바로 그 함정을 피함) higher-half 공유
// 매핑을 통해 **모든** 프로세스 PML4에서 항상 유효하다 - 예전 부팅
// 스택(어느 PML4에도 안 들어있던 lower-half identity map)과 정확히
// 대비되는 지점. 크기는 `Task`의 기본 커널 스택 크기와 동일하게
// 맞췄다(`kTaskDefaultKernelStackSize`, task.h) - 특별한 근거로 고른
// 값이 아니라 이 프로젝트의 기존 커널 스택 기본값을 그대로 재사용한
// 것뿐(실측 후 조정 가능, RM-23F4B687 §4).
alignas(16) uint8_t gIdleStack[kMaxCores][kTaskDefaultKernelStackSize];

// [신규, 2026-09-20, PN-81E49523 2단계] `gIdleTask[coreIndex].tcb`
// 전용 고정 블록 - 일반 Task/AsyncTask는 이제 이 블록을 Slab에서
// 할당하지만(설계자 지시 - 커널 스택과 완전히 분리), `enterIdleLoop()`
// 은 위 `gIdleStack`과 똑같은 이유(부팅 극초반, Slab 할당자가 아직
// 준비되지 않았을 수 있는 시점)로 Slab을 쓸 수 없다 - 그래서 이
// TaskTcb만은 예외적으로 `gIdleStack`과 같은 정적 배열(BSS)로 둔다.
TaskTcb gIdleTaskTcb[kMaxCores];

// [신규, 2026-09-20, PN-81E49523 2단계] `enterIdleLoop()`이 부팅 스택을
// 영원히 버리며 `kContextSwitch`를 호출할 때 "저장은 되지만 다시는
// 안 읽힐" `*oldTcbSlot` 쓰기 대상 - `kContextSwitch`의 저장 절반이
// 이제 `*oldTcbSlot`이 가리키는 자리에 실제로 `mov`로 써야 하므로(더
// 이상 `push`가 알아서 스택에 쌓아 주지 않음), 널 포인터가 아니라
// 반드시 유효한 쓰기 가능 메모리를 가리켜야 한다 - gIdleTaskTcb와
// 같은 이유로 Slab 대신 정적 배열.
TaskTcb gDiscardedBootTcb[kMaxCores];

// 이 코어에서 지금 실행 중인 Task - runLoop()/onTick()/yieldCurrent()
// 만 갱신한다. [갱신, 2026-09-19, PN-D47FBB8D] `nullptr`은 이제 오직
// 부팅 극초반(`startTickOnThisCore()` 호출 이후, 이 코어의
// `enterIdleLoop()`이 아직 `gIdleTask[coreIndex]`를 구성하기 전)의
// 좁은 창에서만 나타나는 과도기 값이다 - 그 창을 지나면 항상
// `&gIdleTask[coreIndex]`(idle/리액터) 아니면 실제 Task를 가리키고,
// 다시는 bare `nullptr`로 되돌아가지 않는다.
Task* gCurrentTask[kMaxCores] = {};

// [SP-9F1DB1D8, QU-68D76FC4/QU-E847DB03] gCurrentTask[]의 모든 접근을
// 보호한다 - 슬롯마다 하나(코어 간 경합 자체가 없으므로 전역 락 하나로
// 묶을 이유 없음). 쓰기는 항상 그 코어 자신만 하지만(같은 코어 접근은
// 레이스가 아니라는 §1/§3 논증에도 불구하고, 설계자 답변으로 예외 없이
// 전부 이 락을 타도록 확정 - SP-9F1DB1D8 §7).
RwSpinlock gCurrentTaskLock[kMaxCores];

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

// [신규, 2026-09-20, PN-584DB994/DC-06FC78E8, 설계자 답변(QU-D14FF560)
// "전용 재진입 플래그 신설"] `onTick()`/`onForcedMigration()`은 EOI를
// 보낸 직후(=이 코어가 다시 인터럽트를 받을 수 있는 상태)부터
// `kContextSwitchFromISR()`로 실제 전환하기 전까지 `gCurrentTask[
// coreIndex]` 등 전역 디스패치 상태를 만진다 - 바로 위 주석의 "인터럽트
// 게이트라 재진입 없음"이라는 전제가 이 좁은 창에는 적용되지 않는다
// (그 전제는 EOI를 늦게 보내 인터럽트 자체가 아예 안 오는 일반적인
// ISR에 대한 것). `gPreemptDisableCount`는 "남이 이 코어의 선점을
// 잠깐 막아 달라"는 요청용이라 재사용할 수 없다(`kContextSwitchFromISR`
// 가 이 Task가 나중에 다시 뽑힐 때에야 "반환"해, enablePreemption()을
// 그 뒤에 두면 이 코어의 스케줄러 틱 자체가 임의로 길게 막힌다 -
// DC-06FC78E8 본문 참고) - 그래서 별도 전용 플래그를 둔다. 아래
// `DispatchWindowGuard`가 이 플래그를 관리한다: 이미 서 있으면(=같은
// 코어에서 이 창이 중첩됨) 이번 인터럽트의 스케줄링 결정 자체를
// 포기하고(EOI는 이미 보냈으니 인터럽트 자체는 정상 처리된 것으로
// 취급) 그냥 반환 - 이 좁은 창에서는 안전하게 재진입을 처리할 방법이
// 없으므로 이번 틱/IPI을 버리는 게 유일한 선택이다.
bool gInDispatchWindow[kMaxCores] = {};

// EOI 직후~`kContextSwitchFromISR()` 호출 직전까지의 재진입 보호
// 구간을 RAII로 관리한다 - `onTick()`/`onForcedMigration()` 양쪽에서
// 동일하게 쓴다. 생성자가 이미 서 있는 플래그를 발견하면 아무 것도
// 세우지 않고 `acquired()==false`를 남긴다(호출부가 이 경우 즉시
// 반환해야 함) - 소멸자는 "이 인스턴스가 실제로 세운 경우에만" 내려
// 이중 해제를 막는다. `release()`를 `kContextSwitchFromISR()` 호출
// **직전**에 명시적으로 불러 그 시점부터는(=실제로 다른 스택/Task로
// 넘어간 뒤부터는) 재진입을 다시 허용한다 - 그 전에 함수가 그냥
// return하는 경로(예: "current 멀쩡함, next 없음")는 소멸자가 대신
// 내려 준다.
class DispatchWindowGuard {
public:
    explicit DispatchWindowGuard(uint32_t coreIndex) : coreIndex_(coreIndex) {
        if (!gInDispatchWindow[coreIndex_]) {
            gInDispatchWindow[coreIndex_] = true;
            acquired_ = true;
        }
    }
    ~DispatchWindowGuard() { release(); }
    DispatchWindowGuard(const DispatchWindowGuard&) = delete;
    DispatchWindowGuard& operator=(const DispatchWindowGuard&) = delete;

    bool acquired() const { return acquired_; }
    void release() {
        if (acquired_) {
            gInDispatchWindow[coreIndex_] = false;
            acquired_ = false;
        }
    }

private:
    uint32_t coreIndex_;
    bool acquired_ = false;
};

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
// **[정정, 2026-09-17, PN-2008220B]** 위 문단의 "runLoop()의 idle이
// 저지대 부팅 스택 위에 있을 수 있다"는 전제 자체가 이제는 사실이
// 아니다 - `Scheduler::enterIdleLoop()`이 부팅 극초반에 이 코어의
// idle 컨텍스트를 higher-half 전용 스택(`gIdleStack`, 모든 PML4에
// 공유)으로 영구히 옮겨 놓는다(아래 `enterIdleLoop()` 문서 주석
// 참고) - 그래서 이제는 runLoop()에서 CR3를 바꿔도 "다음 스택
// 접근이 즉시 폴트"라는 위험 자체는 사라졌다. **그래도 이 함수를
// runLoop()에서 부르지 않는다는 정책은 그대로 유지한다** - SP-83A07867
// 이 CR3 동기화 지점을 onTick()/kTaskStartTrampoline/yieldCurrent/
// parkCurrent 넷으로 신중하게 통합해 둔 기존 아키텍처를 이 무관한
// 수정(스택 안전성) 하나 때문에 재검토하는 건 별도의(그리고 더 위험한)
// 설계 결정이라 이 계획의 범위 밖으로 남겨 둔다 - "이제 안전해졌다"는
// 사실만 기록해 두고, 실제로 정책을 바꿀지는 필요해질 때 별도로
// 판단한다.
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

// SP-83A07867 §3.2 갈래②가 다룬 "Task가 자기 자신의 안전한 스택으로
// 넘어온 뒤 kSyncCr3로 동기화"라는 원칙은 idle로 "돌아오는" 세 지점
// (yieldCurrent/parkCurrent/retireCurrentTask)의 **재개** 지점에만
// 적용됐고, 그 세 함수가 idle로 **떠나는** 순간 자체는 다루지 않은 채
// 남아 있었다(PN-57CF48DB, 2026-09-16 QEMU -d int 트레이스 + gdb로
// 실측 확인) - `kContextSwitch(&current->savedRsp, gIdleSavedRsp[...])`
// 는 아직 current 자신의(안전한) 스택 위에서 RSP만 gIdleSavedRsp로
// 바꾼 뒤 그 자리에서 즉시 pop을 시작하는데, current가 실제
// UserThread면 CR3가 여전히 그 자신의 userPml4Phys라 gIdleSavedRsp
// (부팅 스택, 어느 프로세스의 PML4에도 안 들어있음 - PN-58501EAA)에
// 대한 이 최초 pop 자체가 스택 접근 Page Fault -> 그 폴트 전달마저
// 같은 깨진 스택 위에서 실패해 Double Fault로 이어졌다. 이 함수는
// current 자신의 스택(higher-half, 모든 프로세스가 공유)이 여전히
// 매핑돼 있는 지금 이 시점에 미리 CR3를 gBootPml4Phys로 되돌려 그
// 전제 자체를 없앤다 - kSyncCr3와 동일한 skip-if-same 최적화 패턴.
void kSyncCr3ForIdleTransition() {
    if (Paging::currentPml4Phys() != gBootPml4Phys) {
        asm volatile("mov %0, %%cr3" : : "r"(gBootPml4Phys) : "memory");
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

// [신규, 2026-09-17, SP-9A6D579F §3.4] kSyncCr3/kSyncFpu와 같은 다섯
// 지점(SP-83A07867 §3.2가 확립한 "코어 소유가 아니라 Task 소유
// 레지스터는 디스패치 시점에 동기화" 패턴의 네 번째 훅)에서 호출된다 -
// DR0-3/DR7은 CPU 코어 레지스터이지 Task별 저장 슬롯이 아니므로,
// 이 Task가 소유한 `Process::debugSession`이 요구하는 값으로 매
// 디스패치마다 다시 실어야 한다.
//
// kSyncCr3의 "달라졌을 때만 쓰는" skip-if-same 최적화를 의도적으로
// 안 쓴다 - 직전에 실행됐던 다른 Task가 걸어 둔 하드웨어 브레이크
// 포인트가 코어 레지스터에 그대로 남아 있으면, 이번에 디스패치되는
// Task의 코드/데이터가 우연히 같은 가상주소에 있을 때 전혀 무관한
// 프로세스에 #DB가 잘못 전달될 위험이 있다(레지스터 값이 진짜로
// 코어에 남기 때문) - 그래서 디버그 세션이 없는 흔한 경우를
// 포함해서 항상 명시적으로(디버그 세션이 없으면 전부 0으로) 다시
// 쓴다. `mov to/from dr*`가 다른 레지스터보다 비싼 연산이라는 점은
// 알고 있으나, 디스패치 경계(스케줄러 퀀텀 단위)에서만 일어나는
// 빈도라 지금은 정확성을 우선한다(실측 후 조정 가능, RM-23F4B687 §4).
void kSyncDebugRegs(Task* task) {
    uint64_t dr0 = 0, dr1 = 0, dr2 = 0, dr3 = 0, dr7 = 0;
    if (task->isUserLevel) {
        auto* thread = static_cast<UserThread*>(task);
        if (SharedPtr<Process> proc = thread->process.lock()) {
            // [신규, 2026-09-19, PN-EA968DF0 근본 원인 수정] `pausedByDebugger`
            // (all-stop, 이미 어떤 스레드 하나가 브레이크포인트에 걸려
            // 정지했다는 process-wide 표시)가 세워진 동안은 이 프로세스의
            // 어느 스레드가 디스패치되든 하드웨어 브레이크포인트를 전부
            // 비활성으로 싣는다(dr7=0 그대로 유지, 아래 for 루프를 건너뜀).
            // **근거**: 코어당 하나뿐인 #DB용 IST4 스택은 고정 최상단
            // 주소로 매번 리셋되는 하드웨어 자원이라, 이미 한 스레드가
            // `parkCurrent()`로 그 위에 얼어붙어 있는 동안(DebugContinue가
            // 아직 write-back/재개하지 않은 동안) 같은 코어에서 형제
            // 스레드가 같은(또는 다른) 브레이크포인트를 또 히트하면 그
            // 얼어붙은 호출 체인의 스택 메모리(InterruptFrame 자체뿐
            // 아니라 그보다 더 깊은 kSaveDebugRegistersSnapshot/
            // parkCurrent/kContextSwitch의 저장된 콜리세이브 레지스터·
            // 반환 주소까지)를 덮어써 버린다 - write-back은 InterruptFrame
            // 필드만 복구할 뿐 그 아래 깊이의 손상은 복구하지 못한다(실측
            // 확인: Logger로 TEMP dbg-hit/dbg-writeback 이벤트를 직접
            // 관찰해 같은 코어의 같은 IST4 주소로 두 스레드가 동시에
            // 몰리는 것과, 그 직후 그 주소 바로 아래 오프셋에서 Invalid
            // Opcode가 나는 것을 확인했다 - PN-EA968DF0 본문 참고). 이미
            // 한 스레드가 정지한 이상 다른 형제 스레드가 하드웨어
            // 브레이크포인트에 또 걸릴 필요가 없다(all-stop 모델 자체가
            // "하나가 멈추면 전부 곧 멈춰야 한다"는 의미이므로) - 대신
            // `Scheduler::onTick()`의 기존 지연 경로(`kIsPausedByDebugger`,
            // 그 스레드 자신의 평범한 커널 스택 위 프레임을 씀 - IST4와
            // 무관해 이 경합이 성립하지 않음)가 곧 그 형제 스레드도 안전하게
            // 정지시킨다. **DR7이 완전히 꺼지므로 이 창 동안은 진짜
            // 브레이크포인트 조건에 도달해도 트랩되지 않지만**, 이미
            // all-stop 상태라 유저 입장에서 "곧 멈출 스레드가 브레이크포인트를
            // 한 번 더 정확히 찍었는지"는 관측 대상이 아니다(다음
            // DebugContinue 이후 다시 정상 동작).
            if (proc->debugSession.active && proc->debugSession.pausedByDebugger.load() == 0) {
                uint64_t* const slots[kMaxDebugBreakpoints] = {&dr0, &dr1, &dr2, &dr3};
                for (uint32_t i = 0; i < kMaxDebugBreakpoints; ++i) {
                    const DebugBreakpoint& bp = proc->debugSession.breakpoints[i];
                    if (!bp.enabled) {
                        continue;
                    }
                    *slots[i] = bp.address;
                    dr7 |= (1ULL << (i * 2));  // Li(로컬 인에이블)
                    uint64_t rw = 0;
                    switch (bp.condition) {
                        case DebugBreakpoint::Condition::Execute:
                            rw = 0b00;
                            break;
                        case DebugBreakpoint::Condition::Write:
                            rw = 0b01;
                            break;
                        case DebugBreakpoint::Condition::ReadWrite:
                            rw = 0b11;
                            break;
                    }
                    dr7 |= rw << (16 + i * 4);   // R/Wi
                    // LENi = 00(1바이트) 고정 - v1은 폭 확장을 다루지
                    // 않는다(SP-9A6D579F가 명시하지 않은 세부, 필요해지면
                    // DebugBreakpoint에 길이 필드 추가).
                }
            }
        }
    }
    asm volatile("mov %0, %%dr0" : : "r"(dr0));
    asm volatile("mov %0, %%dr1" : : "r"(dr1));
    asm volatile("mov %0, %%dr2" : : "r"(dr2));
    asm volatile("mov %0, %%dr3" : : "r"(dr3));
    asm volatile("mov %0, %%dr7" : : "r"(dr7));
}

// Push/Pull 로드밸런싱(PN-04D6197A, SP-9525C4C0 §3) - excludeCore를
// 뺀 나머지 코어 중 gNormalQueues 근사 길이가 가장 긴 코어를 O(코어
// 수) 선형 스캔으로 찾는다(Pull이 "훔쳐올 대상"을 고를 때 쓴다).
// 코어 수가 매우 많아지면 이 선형 스캔도 실측 후 재검토(SP-0666DB3C
// §12가 이미 지적한 것과 같은 성격의 트레이드오프, RM-23F4B687 §4
// 취지 - 별도 DC 불필요). 다른 모든 코어의 gNormalQueues가 비어
// 있으면(bestLen이 0에서 갱신되지 않으면) excludeCore 자신을 그대로
// 반환해 "훔쳐올 곳이 없다"를 호출부가 `victimCore == coreIndex`
// 비교 하나로 판정할 수 있게 한다.
uint32_t kFindMostLoadedCore(uint32_t excludeCore) {
    uint32_t best = excludeCore;
    uint32_t bestLen = 0;
    for (uint32_t i = 0; i < gCoreCount; ++i) {
        if (i == excludeCore) {
            continue;
        }
        const uint32_t len = gNormalQueues[i].approxLength();
        if (len > bestLen) {
            bestLen = len;
            best = i;
        }
    }
    return best;
}

// [신규, PN-9DDFB774, SP-9525C4C0 §2.3] Pull(훔쳐오는 idle 코어
// 자신)이 자기 노드를 기준으로 같은 노드 중 가장 바쁜 코어를 먼저
// 찾는다 - Push와 달리 "그 노드 안에 훔칠 만한 게(bestLen>0) 있는지"
// 로 폴백을 가른다(단순히 "다른 코어가 없다"가 아니라 "같은 노드가
// 전부 유휴라 훔칠 게 없다"도 폴백 대상 - kFindMostLoadedCore 원안의
// "bestLen==0이면 못 찾음" 관례를 그대로 재사용). 같은 노드에서 못
// 찾으면 전체 스캔(kFindMostLoadedCore)으로 폴백해 유휴 코어가 다른
// 바쁜 노드의 부하를 놓치지 않게 한다.
uint32_t kFindMostLoadedCoreNumaAware(uint32_t excludeCore) {
    const uint32_t myNode = Acpi::cpuNumaNode(excludeCore);
    uint32_t best = excludeCore;
    uint32_t bestLen = 0;
    for (uint32_t i = 0; i < gCoreCount; ++i) {
        if (i == excludeCore || Acpi::cpuNumaNode(i) != myNode) {
            continue;
        }
        const uint32_t len = gNormalQueues[i].approxLength();
        if (len > bestLen) {
            bestLen = len;
            best = i;
        }
    }
    if (bestLen > 0) {
        return best;
    }
    return kFindMostLoadedCore(excludeCore);
}

// kFindMostLoadedCore와 정확히 대칭 - Push(§2)가 "밀어 넣을 대상"을
// 찾는 데 쓴다. 다른 모든 코어가 이미 excludeCore만큼(또는 그 이상)
// 차 있으면(bestLen이 초기값에서 갱신되지 않으면) excludeCore 자신을
// 그대로 반환한다.
uint32_t kFindLeastLoadedCore(uint32_t excludeCore) {
    uint32_t best = excludeCore;
    uint32_t bestLen = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < gCoreCount; ++i) {
        if (i == excludeCore) {
            continue;
        }
        const uint32_t len = gNormalQueues[i].approxLength();
        if (len < bestLen) {
            bestLen = len;
            best = i;
        }
    }
    return best;
}

// [신규, PN-9DDFB774, SP-9525C4C0 §2.3, QU-759C9C1C 답변 "(A) 참고함"]
// Push가 이관하려는 Task의 numaNode와 같은 노드의 코어들만 먼저
// 스캔해 그중 가장 한가한 코어를 고른다 - "필터로 배제"가 아니라
// "우선순위 2단계"라, 그 노드에 excludeCore 말고 다른 코어가 아예
// 없을 때만(foundCandidate == false) 원안(kFindLeastLoadedCore, 전체
// 스캔)으로 폴백한다 - 같은 노드 후보가 있으면 설령 어느 정도 차
// 있어도(폴백 기준은 "없음"이지 "바쁨"이 아님) 그 노드 안에서만
// 고른다(로컬리티 우선 원칙). SRAT 없는 환경(모든 코어가 노드 0)
// 에서는 이 필터가 사실상 전체 스캔과 같아져 원안과 동일하게
// 동작한다.
uint32_t kFindLeastLoadedCoreNumaAware(uint32_t excludeCore, uint32_t taskNumaNode) {
    uint32_t best = excludeCore;
    uint32_t bestLen = 0xFFFFFFFFU;
    bool foundCandidate = false;
    for (uint32_t i = 0; i < gCoreCount; ++i) {
        if (i == excludeCore || Acpi::cpuNumaNode(i) != taskNumaNode) {
            continue;
        }
        foundCandidate = true;
        const uint32_t len = gNormalQueues[i].approxLength();
        if (len < bestLen) {
            bestLen = len;
            best = i;
        }
    }
    if (foundCandidate) {
        return best;
    }
    return kFindLeastLoadedCore(excludeCore);
}

// SP-9525C4C0 §2 Push 임계치의 "최대 길이" - QU-9325BD40 설계자 답변
// (2026-09-16, "설정가능한 최대/최소값 + 코어 수 대비 상대적 기준")을
// 그대로 구현한 것. `TaskQueue`는 원래 무제한 연결리스트라 "최대
// 길이"에 대응하는 고정값이 코드에 없었다 - 코어가 많을수록 시스템
// 전체가 소화할 수 있는 총 대기열도 자연히 늘어난다는 전제로 "코어당
// 기준값 x 코어 수"를 상대적 기준으로 삼되, 코어가 하나뿐이거나
// 극단적으로 많을 때도 문턱값이 비합리적으로 작거나 커지지 않도록
// 설정 가능한 min/max 상수로 clamp한다.
constexpr uint32_t kNormalQueueLengthPerCore = 8;
constexpr uint32_t kNormalQueueMinLength = 8;
constexpr uint32_t kNormalQueueMaxLength = 64;
constexpr uint32_t kPushThresholdPercent = 90;

uint32_t kEffectiveNormalQueueMaxLength() {
    uint32_t maxLength = kNormalQueueLengthPerCore * gCoreCount;
    if (maxLength < kNormalQueueMinLength) {
        maxLength = kNormalQueueMinLength;
    }
    if (maxLength > kNormalQueueMaxLength) {
        maxLength = kNormalQueueMaxLength;
    }
    return maxLength;
}

// Push(§2)가 실제로 이관을 발동할 문턱값 - 위 유효 최대 길이의 90%.
uint32_t kPushThresholdLength() {
    return (kEffectiveNormalQueueMaxLength() * kPushThresholdPercent) / 100;
}

// [신규, 2026-09-17, SP-B26CDBDD §2.1] 표면 가중치([-100,100], 기본 0)를
// vruntime 나눗셈에 쓸 수 있는 실효 가중치(항상 양수)로 바꾼다 -
// 선형 변환(effectiveWeight = 100 + weight), 0으로 나누기 방지를 위해
// 최솟값 1로 클램프.
constexpr int32_t kEffectiveWeightBase = 100;   // weight=0(기본값)일 때의 실효 가중치
constexpr int32_t kMinEffectiveWeight = 1;
int32_t kEffectiveWeightOf(int32_t weight) {
    const int32_t raw = kEffectiveWeightBase + weight;  // [-100,100] -> [0,200]
    return raw < kMinEffectiveWeight ? kMinEffectiveWeight : raw;
}

// [신규, 2026-09-17, SP-B26CDBDD §2.1, QU-DA6C52BA 답변 "수학적 동치값"
// 고정소수점 스케일] `kEffectiveWeightBase / kEffectiveWeightOf(weight)`를
// 스케일 없이 정수 나눗셈하면 실효 가중치가 100보다 큰 모든 경우
// (weight > 0 - 이 기능이 존재하는 이유인 바로 그 경우)에 몫이 0으로
// 버려져 vruntime이 전혀 안 늘어나는 실질적 버그가 된다(그 Task가
// 무한정 우선 실행됨, 의도와 정반대). 분자를 미리 이 배율만큼 키워
// 정수 나눗셈이어도 원래 실수 몫의 정보를 보존한다 - 모든 Task가
// 항상 같은 스케일을 쓰므로 OrderedList(§3)의 상대 순서 비교에는
// 전혀 영향 없다(값 자체가 아니라 값들 사이의 순서만 의미가 있다는
// CFS 불변조건과 정확히 부합).
constexpr uint64_t kVruntimeScale = 1024;  // 2^10 - 실효 가중치 최댓값(200)까지도 몫의 소수부를 충분히 보존

// SP-9525C4C0 §5.3/§6-항목3(2026-09-15 설계자 확정) - 코어 간 이관
// 앞에서 FPU 소유권을 안전하게 넘길 수 있는지 판정한다. **여기서
// 실제로 FXSAVE를 하지 않는다** - §5.3이 밝힌 대로 FXSAVE는 항상
// "이 명령을 실행하는 코어"의 하드웨어 레지스터만 읽으므로, 로드밸런싱을
// 수행 중인 코어(Push를 받는 쪽이든 Pull로 훔쳐오는 쪽이든)가 원격
// fromCore를 대상으로 직접 FXSAVE하면 엉뚱한(자기 자신의) 레지스터를
// 저장하는 조용한 데이터 손상 버그가 된다 - IPI로 fromCore 자신에게
// 위임하는 방법도 있지만 매 이관마다 왕복 비용이 커 v1은 채택하지
// 않는다(§6-항목3 확정). 대신 **이미 안전이 보장된 경우에만 이관을
// 허용**한다: 이 Task가 FPU를 한 번도 안 썼거나(`!fpuContext`),
// 이미 다른 소유자에게 넘어가 `fpuContext`가 최신인 것이 보장된 경우
// (`gFpuOwner[fromCore] != task`) - 그렇지 않으면(정말 이 Task가
// fromCore의 살아있는 FPU 소유자) false를 반환해 호출부가 이번 이관을
// 보류(스킵)하게 한다.
bool kCanMigrateFpuSafely(const Task* task, uint32_t fromCore) {
    return !task->fpuContext || gFpuOwner[fromCore] != task;
}

// [신규, PN-F55FB154] Push가 targetCore로 이관하기 전에, task가 지금
// "어느 코어에서든" 살아있는 FPU 소유자인지 전체 스캔으로 확인한다.
// Pull(runLoop)의 victimCore는 그 Task를 방금 popFront()한 바로 그
// 큐라 kCanMigrateFpuSafely(task, victimCore)가 의미상 정확하지만,
// Push의 coreIndex 인자는 그런 보장이 없다 - 실제 호출부 전수 조사
// (debug_session.cpp/kmain.cpp ×2/process.cpp/resource_group.cpp)
// 결과 다섯 곳 전부 Scheduler::currentCoreIndex()(이 Task를 지금
// 깨우는 코어, 즉 waker 자신)만 넘기고, "이 Task가 마지막으로 FPU를
// 쓴 코어"와는 무관하다. 그래서 kCanMigrateFpuSafely(task, coreIndex)
// 를 그대로 재사용하면 거의 항상 엉뚱한 코어의 gFpuOwner만 검사해
// 실제로는 위험한 이관도 "안전"으로 오판하게 된다 - 대신 gFpuOwner[]
// 전체를 스캔해 task가 실제로 살아있는 소유자인 코어를 직접 찾는다.
// 찾지 못하면(어디서도 소유자가 아니면) gCoreCount를 반환한다(유효
// 코어 인덱스는 항상 <gCoreCount이므로 이 값이 안전한 "없음" sentinel).
uint32_t kFindFpuOwnerCore(const Task* task) {
    for (uint32_t i = 0; i < gCoreCount; ++i) {
        if (gFpuOwner[i] == task) {
            return i;
        }
    }
    return gCoreCount;
}

// [SP-ECC59BAE §3.1] 강제 이관 요청 슬롯 - tlb_shootdown.cpp의
// g_tlbShootdownRequest 단일 슬롯과 동일한 관례(PN-D132A1E9가 그
// 문서에 남긴 "동시 호출자가 여럿이면 슬롯을 늘리거나 직렬화 락이
// 필요하다"는 경고도 그대로 유효 - v1은 자동 트리거가 없어(§5 (C))
// 호출부가 사실상 하나뿐이라 문제되지 않는다).
struct ForcedMigrationRequest {
    Task* target = nullptr;
    uint32_t targetCore = 0;
};
ForcedMigrationRequest gForcedMigrationRequest;

// [SP-ECC59BAE §4] kCanMigrateFpuSafely()와 달리 이 함수는 "지금
// 당장 fromCore 자신에서 실행되는" IPI 핸들러 안에서만 호출된다 -
// FXSAVE는 항상 그 명령을 실행하는 코어의 하드웨어 레지스터만
// 읽으므로, fromCore 자신이 직접 부르는 한 원격 코어를 잘못
// FXSAVE하는 문제(kCanMigrateFpuSafely 문서 주석 참고)가 애초에
// 생기지 않는다 - Ready 큐 이관(Push/Pull)처럼 "허용된 경우에만
// 이관"으로 회피할 필요 없이, 살아있는 FPU 상태를 그 자리에서 바로
// 안전하게 반납(evict)할 수 있다.
void kEvictFpuBeforeMigration(Task* task, uint32_t fromCore) {
    if (gFpuOwner[fromCore] == task) {
        // task가 gFpuOwner인 이상 handleFpuTrap()을 이미 거쳤으므로
        // fpuContext는 항상 non-null이다(이 조건 자체가 그 불변조건).
        asm volatile("fxsave (%0)" : : "r"(task->fpuContext->buffer) : "memory");
        gFpuOwner[fromCore] = nullptr;
    }
}

// kLoadBalanceWakeVector의 ISR - hlt에서 깨우는 것 자체가 목적이라
// 몸체가 필요 없다(tlb_shootdown.cpp의 "EOI는 kIsrHandler가 대신
// 보낸다" 관례 그대로).
void kLoadBalanceWakeIsr(InterruptFrame*) {}

// SP-9525C4C0 §4 - Push가 다른 코어 큐에 Task를 밀어 넣은 뒤, 그
// 코어가 지금 hlt로 잠들어 있을 가능성이 높으면 즉시 깨운다.
// `gCurrentTask[coreIndex] == nullptr`은 정확한 판정이 아니지만
// (idle 진입 직전/직후의 좁은 창) 안전한 근사다 - 이미 실행 중인
// 코어에 괜히 IPI를 보내도 무해한 인터럽트 하나 처리하고 마는 것뿐
// (기존 스케줄러 틱과 동일하게 EOI 후 즉시 반환)이라 false positive
// 비용이 낮다.
void kWakeCoreIfIdle(uint32_t coreIndex) {
    RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
    if (gCurrentTask[coreIndex] == nullptr) {
        Lapic::sendFixedIpi(Acpi::cpuApicId(coreIndex), static_cast<uint8_t>(kLoadBalanceWakeVector));
    }
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
// 캐스팅할 수 있다.
//
// [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] **이 핸들러는 지금도
// "이 스레드가 끝나는 순간이 곧 Process 전체가 끝나는 순간"이라고
// 가정한 채 무조건 `process->destroy()`를 부른다 - `Process::threads`
// (process.h)가 여러 스레드를 담을 수 있게 됐지만, 그 목록에서 정말
// 여러 스레드가 동시에 살아있는 경로(`CreateThread` syscall)가 아직
// 없어 오늘은 이 가정이 여전히 항상 참이다.** `SelfTerminateThread`
// syscall(SP-76250478 §3 항목2, 후속 증분)이 착수되면 이 가정이 깨진다
// - 그 syscall이 착수될 때 이 핸들러의 자연 종료 경로(`kTaskFallingToEnd`
// 전용, 위 kSyscallEndpointSelfTerminate 문서 참고)는 "프로세스 전체를
// 강제 종료"(§3 항목1, 미처리 예외 경로)로 의미가 좁혀져야 하고,
// 정상적인 개별 스레드 종료는 별도 핸들러가 `userThread`를
// `process->threads`에서 좀비 표시만 하고 `destroy()`는 그 목록이
// 실제로 비었을 때만 호출해야 한다(process.cpp WaitHandler의 좀비
// 스레드 회수 로직과 대칭). `process` 필드는 execImage()가 항상 채워
// 두지만(process.cpp의 `thread->process = weakFromThis();`) 방어적으로
// null 확인한다.
// §6.4 비필수 서비스 재스폰 예약(DelayedExecutionQueue::schedule)의
// 콜백 인자 - `respawn`(프로세스별 고정 스폰 헬퍼)과
// `consecutiveFailures`(그 헬퍼가 새 Process에 그대로 이어 담을 값)를
// 한 번에 실어 날라야 하는데 DelayedCallback은 void* 인자 하나뿐이라
// slab에서 이 작은 구조체 하나를 빌려 온다(AsyncTask 자신의 콜백 인자
// 전달 관례와 동일).
struct ResurrectSpawnArgs {
    void (*respawn)(uint32_t consecutiveFailures);
    uint32_t consecutiveFailures;
};

void kResurrectSpawnTrampoline(void* arg) {
    auto* args = static_cast<ResurrectSpawnArgs*>(arg);
    args->respawn(args->consecutiveFailures);
    GenericSlabAllocator::free(args, sizeof(ResurrectSpawnArgs));
}

// [신규, 2026-09-18, SP-76250478 §3, PN-0EB2FABF] `SelfTerminateHandler`
// (call0, "이 스레드가 끝나면 프로세스 전체가 끝난다" - POSIX `exit()`
// 의미)와 `SelfTerminateThreadHandler`(call9, 아래, "process->threads가
// 실제로 비었을 때만" 같은 마무리가 필요 - POSIX 마지막 스레드가
// `pthread_exit()`한 경우와 동일 의미) 양쪽이 공유하는 프로세스 종료
// 마무리 로직 - 원래 `SelfTerminateHandler::onExec()` 하나에만 있던
// 코드를 순수 이동한 것뿐(동작 변화 없음, 두 번째 호출부가 생겨 뺐다).
// `process->destroy()`(주소공간 반납)부터 고아 입양/좀비 마킹/essential
// 패닉/resurrect 예약까지 전부 담당한다.
void kFinalizeProcessTermination(SharedPtr<Process>& process) {
    // Resurrect(SP-EAB162FC §6, 2026-09-16 §6.3/§6.4 개정 반영) -
    // destroy() 이후에도 Process 객체 자체(캐스팅 근거: 정적/
    // 장기수명 인스턴스 - destroy()는 주소공간만 반납할 뿐 이
    // 구조체를 지우지 않는다)는 살아있어 startFlags/
    // consecutiveFailures를 안전하게 읽을 수 있다. 재스폰은
    // 기존 주소공간이 완전히 반납된 뒤에 한다(자원 회수 -> 재생성
    // 순서).
    const ProcessStartFlags startFlags = process->startFlags;
    const uint32_t newConsecutiveFailures = process->consecutiveFailures + 1;
    process->destroy();

    // [신규, 2026-09-16, SP-6BEAE0C1 §6, PN-543C0CE9 착수 5번째
    // 증분(2/2)] 고아 입양 - 이 프로세스 자신이 SpawnProcess로
    // 자식을 만들어 뒀다면(parent가 비어 있는 고정 스폰
    // KernelService라도 스스로 SpawnProcess를 부를 수 있다 -
    // devmgr가 PnP 드라이버 자식을 만드는 경우 등), 그 자식들은
    // 이제 부모를 잃는다. §6 "고아는 init이 입양"에 따라
    // 살아있는 자식이든 이미 좀비인 자식이든 전부 orphanRoot()로
    // reparent한다 - 좀비 자식은 reparent만 하고 실제 회수(reap)는
    // 여전히 init이 나중에 wait()를 불러야 하는 채로 남는다(이
    // 시점에 자동으로 회수하지 않는다 - §11-2 참고, "init이
    // 실제로 wait()를 자동 반복 호출하는지"는 커널이 강제할
    // 정책이 아니라 유저랜드 init 구현의 몫).
    //
    // [수정, 2026-09-17, PN-E2A114C1] `children`이 이제 자식의
    // 유일한 강한 소유자다 - `orphanRoot->children.insert(child)`
    // 가 그 소유권을 그대로 이어받고(SharedPtr 복사, 강한 참조
    // +1), 뒤이은 `process->children.clear()`가 옛 소유자
    // 쪽 몫을 내려놓는다(chunked_list.h 수정 참고) - net
    // 참조 카운트는 그대로, 소유자만 바뀐다.
    SharedPtr<Process> orphanRoot = Process::orphanRoot().lock();
    if (orphanRoot && orphanRoot.get() != process.get()) {
        process->children.forEach([&](SharedPtr<Process>& child, auto*) {
            if (!child) {
                return;
            }
            child->parent = WeakPtr<Process>(orphanRoot);
            orphanRoot->children.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
            // 실패(슬랩 고갈)해도 그냥 넘어간다 - child->parent는
            // 이미 orphanRoot로 바뀌었으니 그 자식이 나중에 종료할
            // 때 좀비로는 남지만, 이 순간 orphanRoot->children
            // 목록에 못 들어갔다면 root가 그 좀비를 wait()로 찾지
            // 못한다(드문 자원 고갈 경합 - 새 DC 없이 감수할 수준의
            // v1 한계, RM-23F4B687 §4).
            orphanRoot->children.insert(child);
        });
    }
    process->children.clear();

    // 부모가 있으면(SpawnProcess로 만들어진 트리 멤버) 좀비로
    // 남겨 부모의 wait()(RM-48E1E610 35번)를 기다린다 - Process
    // 구조체/threads(process.h)가 담고 있던 UserThread들의 반납은
    // WaitHandler(process.cpp)가 회수
    // 시점에 담당한다(이제 SharedPtr 마지막 강한 참조 소멸을
    // 통해서 - process.cpp WaitHandler 참고). 부모가 없으면
    // (고정 스폰 KernelService, 또는 SpawnProcessHandler 주석의
    // 이론상 도달 불가 경로) 기존과 완전히 동일하게 아무도
    // 회수하지 않는 상태로 그냥 남는다 - `gInitProcess`/
    // `gServiceProcess[]` 전역이 계속 강하게 붙들고 있으므로
    // 이 지역 변수 `process`가 스코프를 벗어나도 파괴되지 않는다.
    if (SharedPtr<Process> parent = process->parent.lock()) {
        process->isZombie = true;
        // exitCode(§6) - 프로세스 트리 좀비의 exitCode는 여전히 0
        // 고정이다(§6이 다루는 건 Process::exitCode, UserThread::
        // exitCode - SelfTerminateThread가 기록하는 값, §3 항목2 - 와는
        // 별개 축이라는 점이 이번 증분으로 더 분명해졌다. 프로세스
        // exitCode에 실제 값을 물려주는 건 §3.2 "마지막 스레드의
        // exitCode를 그대로 물려받는다"의 몫으로 후속 증분(Join 완료
        // 시점)에 배선한다).
        process->exitCode = 0;
        // [신규, 2026-09-19, PN-485132FF, SP-68182FBD §4] 자식 종료를
        // 부모에게 실제로 통지한다 - `kCheckSignalCheckpoint()`(idt.cpp)
        // 가 이제 Kill/Terminate 전용 하드코딩이 아니라 dispositions[]를
        // 실제로 참조하는 범용 체크포인트로 일반화돼(PN-59A60413) 이
        // 신호가 비로소 관찰 가능한 효과를 낸다 - 부모가 Default(기본값)
        // 면 종료, SignalAction으로 Ignore를 설정해 뒀으면 생존.
        parent->raiseSignal(SignalNumber::Chld);
    }

    if (startFlags.essential) {
        // §6.3 1번 - resurrect 값과 무관하게 항상 즉시 패닉("커널
        // 서비스가 죽으면 커널이 정상 동작하지 않는다"는 전제가
        // 그대로 적용되는 쪽). 이 프로세스의 이름은 spawnName이
        // memcpy(exactLength)로만 채워지고 나머지는 정적 초기화로
        // 이미 0(널)이라 항상 안전하게 널종단 문자열로 읽힌다.
        Serial::write("minicore: PANIC - essential service died: ");
        Serial::write(process->spawnName);
        Serial::write("\n");
        kPanic("Essential service died");
    } else if (startFlags.resurrect && startFlags.respawn) {
        // §6.3 2번 - 더 이상 "즉시" 재스폰하지 않는다. §6.4의
        // 지연/백오프 일정(분 단위)을 DelayedExecutionQueue(§2/§3,
        // PN-C46DF296)의 틱 단위로 환산해 예약한다 - 100Hz는
        // timer.h가 문서화한 Timer::tickCount()의 고정 틱 레이트
        // (HPET/PIT 보정 공통, 새 시간원 도입 없이 그대로 재사용).
        constexpr uint32_t kTimerTicksPerSecond = 100;
        const uint32_t intervalMinutes = kResurrectIntervalMinutes(newConsecutiveFailures);
        const uint64_t delayTicks = static_cast<uint64_t>(intervalMinutes) * 60 * kTimerTicksPerSecond;
        auto* args = static_cast<ResurrectSpawnArgs*>(GenericSlabAllocator::alloc(sizeof(ResurrectSpawnArgs)));
        if (args) {
            args->respawn = startFlags.respawn;
            args->consecutiveFailures = newConsecutiveFailures;
            DelayedExecutionQueue::schedule(delayTicks, kResurrectSpawnTrampoline, args);
        }
        // args 할당 실패(슬랩 고갈)면 이번 재스폰 시도 자체를
        // 건너뛴다 - essential=false라 패닉하지 않는다는 정책과
        // 일관되게, 자원 고갈도 "조용히 재시도를 포기"로 처리한다
        // (다음에 이 서비스가 다른 경로로 다시 죽을 때 또 시도됨).
    }
    // 그 외(resurrect==false): 기존과 동일하게 그냥 종료(재스폰
    // 없음, 패닉 없음) - §6.3 3번.
}

class SelfTerminateHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* args) override {
        auto* target = static_cast<Task*>(args);
        Scheduler::retireTask(target);
        auto* userThread = static_cast<UserThread*>(target);

        // [PN-40E976F2, 리팩터링 2026-09-18 PN-B5C2845A] 이 UserThread가
        // 제출했지만(Syscall::submit) 아직 wait()로 소비되지 않은
        // AsyncTask들을 정리한다 - 이제 그 결과를 가져갈 사람이 영원히
        // 없다. 자기 자신이 죽는 경우라 이 목록의 어떤 항목도
        // waitingTask를 갖지 않으므로(cancelPendingSyscalls 문서 참고)
        // 아래 clear()로 목록 전체를 마저 비워도 안전하다.
        Scheduler::cancelPendingSyscalls(userThread);
        userThread->pendingSyscalls.clear();

        // [수정, 2026-09-17, PN-E2A114C1] `userThread->process`가 이제
        // `WeakPtr<Process>`라 진위(truthiness) 검사가 아니라 `.lock()`
        // 으로 유효성을 확인해야 한다 - 그 결과(`process`, 지역
        // `SharedPtr<Process>`)를 이 블록이 끝날 때까지 붙들고 쓴다.
        SharedPtr<Process> process = userThread->process.lock();
        if (process) {
            // [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] 실제 마무리
            // 로직은 `kFinalizeProcessTermination()`(위)으로 뺐다 -
            // 이 핸들러(call0)는 언제나 무조건 프로세스 전체를 끝낸다
            // (POSIX `exit()` 의미, 다른 스레드가 살아있어도 무관 -
            // §3 항목1 미처리 예외/신호/자연 종료가 전부 이 경로).
            kFinalizeProcessTermination(process);
        }
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

SelfTerminateHandler gSelfTerminateHandler;

// [신규, 2026-09-18, SP-76250478 §3 항목2, PN-0EB2FABF] `SelfTerminateThread`
// (syscall.h의 kSyscallEndpointSelfTerminateThread 문서 주석 참고)의
// 실제 핸들러 - `SelfTerminateHandler`와 달리 **이 스레드 하나만**
// 끝낸다(POSIX `pthread_exit()` 의미). args는 `kThreadOnFallingToEnd`
// (아래)가 `Syscall::submitDetached()`로 넘긴, 종료 대상 UserThread
// 자신(Task*)이다 - `SelfTerminateHandler`와 완전히 같은 관례(exitCode는
// 이미 `kThreadOnFallingToEnd`가 `userThread->exitCode`에 심어 뒀다).
class SelfTerminateThreadHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* args) override {
        auto* target = static_cast<Task*>(args);
        Scheduler::retireTask(target);
        auto* userThread = static_cast<UserThread*>(target);

        Scheduler::cancelPendingSyscalls(userThread);
        userThread->pendingSyscalls.clear();

        SharedPtr<Process> process = userThread->process.lock();
        if (!process) {
            co_return;
        }

        // isZombie(§3) - exitCode는 kThreadOnFallingToEnd가 이미 기록.
        userThread->isZombie = true;

        // [신규, 2026-09-18, SP-76250478 §3 항목2, PN-0EB2FABF] 이
        // 스레드는 다시는 실행되지 않으므로(커널 스택은 위 retireTask()
        // 가 이미 회수) 자기 전용 유저 스택도 즉시 돌려준다 - Join
        // 여부와 무관하게(§3.1이 회수하는 건 `UserThread` 구조체 자신
        // 뿐, 스택은 그럴 필요가 없다). `ProcessAddressSpaceManager`가
        // 최대 8개 VMA만 지원하는 희소 자원이라(address_space.h) 좀비
        // 상태로 오래 남아 있어도 이 VMA만은 즉시 반납해야 한다.
        // `execImage()`/fork()가 만든 스레드(threadStackBase==0, 고정
        // 스택)는 여기서 건드리지 않는다 - `destroy()`의 `unmapAll()`이
        // 대신 회수.
        if (userThread->threadStackBase != 0) {
            process->addressSpace.unmapRegion(userThread->threadStackBase, userThread->threadStackSize);
        }

        // [신규, 2026-09-18, SP-76250478 §3.1, PN-0EB2FABF] Join()이
        // 이미 이 스레드를 기다리며 정지해 뒀는지 확인 - process.cpp의
        // `JoinAwaiter`/`JoinHandler` 문서 주석과 정확히 대응하는 짝.
        // `detached`와 `joinerAsyncTask`는 서로 배타적이다(JoinHandler는
        // `detached`인 대상을 거부하고, DetachHandler는 `joinerAsyncTask`
        // 가 이미 걸린 대상을 거부한다 - §3.1 "경쟁 상황이면 에러로
        // 거부") - 그래서 아래를 if/else if로 나눠도 안전하다.
        AsyncTaskWeakRef* joinerRef = userThread->joinerAsyncTask;
        if (joinerRef) {
            userThread->joinerAsyncTask = nullptr;
            // 회수(reap)를 여기서 직접 끝낸다(§3.1 "그 자리에서 직접
            // 회수까지 마치고 완료 처리") - JoinArgs를 먼저 채운
            // 다음에 지운다(재개된 Join 코루틴은 target을 다시 안
            // 건드리므로 순서 자체는 자유롭지만, "결과가 이미 확정된
            // 뒤에만 재개한다"는 계약을 명확히 하기 위해 회수를
            // 재개보다 먼저 한다).
            AsyncTask* joinTask = joinerRef->lock();
            if (joinTask) {
                auto* joinArgs = static_cast<JoinArgs*>(joinTask->args);
                joinArgs->exitCode = userThread->exitCode;
                joinArgs->error = JoinError::None;
            }
            // joinTask가 이미 사라졌어도(조인 호출자가 먼저 죽은 극단적
            // 경쟁 - JoinHandler::onCancel 문서 주석의 알려진 v1 한계)
            // 이 스레드 자신은 정상적으로 회수해야 한다 - 아무도 결과를
            // 못 받을 뿐 UserThread 슬랩이 새면 안 되므로.
            auto* slot = process->threads.find(
                [userThread](const SharedPtr<UserThread>& t) { return t.get() == userThread; });
            if (slot) {
                process->threads.erase(slot);
            }
            UserThread::release(userThread);
            // 이 필드가 쥐고 있던 "joiner 몫" 하나를 내려놓는다(AsyncTask
            // 자신의 몫은 그 AsyncTask가 나중에 반납될 때 별도로 처리).
            joinerRef->release();
            if (joinTask) {
                // preemptive=true - 실제로 파킹된 대기자(Join 호출자
                // 자신이 Syscall::wait()로 재우고 있었을 UserThread)를
                // 깨우는 경로라 PN-4FA5F13B가 확립한 것과 동일한 이유로
                // 즉시 드레인을 강제한다.
                AsyncReactor::submitCompletion(joinTask, /*preemptive=*/true);
            }

            const bool anyThreadLeft = process->threads.find([](const SharedPtr<UserThread>&) { return true; }) !=
                                        nullptr;
            if (!anyThreadLeft) {
                kFinalizeProcessTermination(process);
            }
        } else if (userThread->detached) {
            // §3 항목3 - 좀비 단계를 건너뛰고 그 자리에서 즉시 회수.
            // process.h/syscall.h의 threads 문서 주석 "순서 중요"
            // 그대로 - release() 전에 컨테이너 슬롯부터 지운다.
            auto* slot = process->threads.find(
                [userThread](const SharedPtr<UserThread>& t) { return t.get() == userThread; });
            if (slot) {
                process->threads.erase(slot);
            }
            UserThread::release(userThread);

            // [신규, 2026-09-18, SP-76250478 §3.2, PN-0EB2FABF] "프로세스의
            // 모든 스레드가 종료되는 순간 Process 자신도 좀비화된다" -
            // v1은 main 스레드가 이 syscall을 쓰도록 배선돼 있지 않아
            // (유저랜드 트램폴린은 CreateThread가 만든 스레드 전용) 이
            // 분기가 실제로 마지막 스레드를 지우는 경우는 이론상으로만
            // 있다(누군가 main에서도 이 syscall을 직접 부르는 극단적
            // 사용법) - 그래도 POSIX 시맨틱을 정직하게 지키기 위해
            // 검사한다. ChunkedList에 size()/empty()가 없어 find로
            // 대신 확인.
            const bool anyThreadLeft = process->threads.find([](const SharedPtr<UserThread>&) { return true; }) !=
                                        nullptr;
            if (!anyThreadLeft) {
                kFinalizeProcessTermination(process);
            }
        }
        // 좀비고 detached도 joinerAsyncTask도 없으면: `threads`에 그대로
        // 남겨 둔다 - 나중에 걸릴 `Join`이 회수할 때까지 UserThread
        // 구조체 자신(threadId/exitCode 보관용)만 살려 둔다.
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

SelfTerminateThreadHandler gSelfTerminateThreadHandler;

// [신규, 2026-09-17, SP-B26CDBDD §7, PN-158B6B2F] `SetTaskWeight` -
// 이번 증분은 `targetPid == kSelfTaskWeightPid`(자기 자신) 경로만
// 구현한다. 직계 자식 대상 경로는 `SP-30FCC8AE`(uid/gid, 아직 review)
// 승인 이후 `Uid`/`kIsDescendantUser`가 실제 코드로 존재해야만 착수
// 가능한 하드 의존성이라(§7.1) 그 전까지는 항상 거부(ok=false)한다.
class SetTaskWeightHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SetTaskWeightArgs*>(argsRaw);
        if (args->weight < Task::kMinTaskWeight || args->weight > Task::kMaxTaskWeight) {
            args->ok = false;  // 범위 밖 - 조용히 clamp하지 않고 거부(표준 커널 syscall 관례)
            co_return;
        }
        SharedPtr<Task> submitter = task->submitterTask.lock();
        if (!submitter) {
            args->ok = false;
            co_return;
        }
        if (args->targetPid == kSelfTaskWeightPid) {
            submitter->weight = args->weight;  // 다음 vruntime 갱신(§2.2)부터 즉시 반영
            args->ok = true;
            co_return;
        }
        // [SP-30FCC8AE 승인 이후 구현 - §7.1 하드 의존성] 직계 자식 대상
        // 경로는 Kill(PN-71E50394)과 동일한 스코프(Process::children
        // 순회, kResolveProcessId 불필요)로 uid 권한 검사(caller.uid==
        // root || kIsDescendantUser(target.uid, caller.uid))를 통과해야만
        // 허용된다 - 그 인프라가 아직 없어 지금은 항상 거부.
        args->ok = false;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}  // onExec에 정지 지점 없음 - SP-71DA77B3/PN-BD276A24가 확립한 판단 기준과 동일
};

SetTaskWeightHandler gSetTaskWeightHandler;

}  // namespace

// [신규, 2026-09-18, PN-22E5E9E7 항목3, SP-29D652AA §4.4] kSyncCr3/
// kSyncFpu/kSyncDebugRegs와 정확히 같은 다섯 지점(SP-83A07867 §3.2가
// 확립한 "코어 소유가 아니라 Task 소유 레지스터는 디스패치 시점에
// 동기화" 패턴)에서 호출된다 - FS_BASE(MSR 0xC0000100)는 CR3와 똑같이
// `kContextSwitch`(콜리세이브+RFLAGS만 저장/복원)도 `iretq`(InterruptFrame)
// 도 건드리지 않는 순수 코어 레지스터라, Task 전환마다 이 훅이 명시적으로
// 다시 실어야 진짜 컴파일러 `thread_local`(%fs-상대 접근, task.cpp의
// `Task::init()`이 만든 `kernelFsBase` TCB 블록)이 "지금 실행 중인
// Task 것"을 가리킨다. kSyncCr3의 skip-if-same 최적화(TLB flush 회피가
// 목적)는 여기 해당 없다 - wrmsr(FS_BASE)는 TLB를 건드리지 않아 매번
// 무조건 다시 쓴다(kSyncDebugRegs와 동일한 판단, 정확성 우선).
//
// [갱신, 2026-09-18, PN-22E5E9E7 항목7] 익명 네임스페이스(이 파일
// 내부 전용)에서 꺼내 `kernel::` 진짜 심볼로 승격 - `idt.cpp`의
// `kDispatchSyscallVerb`(syscall/int 0x80 공용 디스패치)가 ring3에서
// 진입한 직후 FS_BASE를 이 Task 자신의 커널 TCB로 되돌리는 데 이
// 함수를 그대로 재사용한다(scheduler.h에 선언 추가).
void kSyncFsBase(Task* task) {
    constexpr uint32_t kIa32FsBaseMsr = 0xC0000100u;
    const uint64_t target = task->kernelFsBase;
    const uint32_t lo = static_cast<uint32_t>(target);
    const uint32_t hi = static_cast<uint32_t>(target >> 32);
    asm volatile("wrmsr" : : "c"(kIa32FsBaseMsr), "a"(lo), "d"(hi) : "memory");
}

// [신규, 2026-09-18, PN-22E5E9E7 항목7, SP-29D652AA §5.3] 위 kSyncFsBase
// 의 유저(ring3) 대응 - 이 UserThread가 실제로 ring3 코드를 실행하려는
// 순간(kEnterRing3의 최초 진입, syscall/int 0x80 처리를 마치고 ring3로
// 돌아가기 직전)에 FS_BASE를 그 스레드 자신의 유저 thread_local
// 인스턴스(`UserThread::userFsBase`, 항목6)로 되돌린다. kSyncFsBase와
// 달리 다섯 디스패치 지점이 아니라 "커널→유저 전환" 지점 전용이다 -
// Task 디스패치(onTick 등)는 여전히 kSyncFsBase만 부른다(그 시점엔
// 아직 ring0이므로 커널 TCB가 맞다) - 이 함수는 그 이후 실제로 ring3에
// 진입/복귀하는 좁은 지점에서만 추가로 불린다. `hasTlsTemplate`이
// false인 v1 유저 바이너리 전부는 `userFsBase`가 0으로 남아 있어
// 그대로 wrmsr(0)이 실리지만, 그 바이너리는 애초에 %fs-상대 접근을
// 컴파일하지 않으므로 무해하다.
void kSyncFsBaseToUser(UserThread* thread) {
    constexpr uint32_t kIa32FsBaseMsr = 0xC0000100u;
    const uint64_t target = thread->userFsBase;
    const uint32_t lo = static_cast<uint32_t>(target);
    const uint32_t hi = static_cast<uint32_t>(target >> 32);
    asm volatile("wrmsr" : : "c"(kIa32FsBaseMsr), "a"(lo), "d"(hi) : "memory");
}

void Scheduler::init() {
    gCoreCount = Acpi::cpuCount();
    if (gCoreCount == 0) {
        gCoreCount = 1;
    }
    if (gCoreCount > kMaxCores) {
        gCoreCount = kMaxCores;
    }
    // [신규, 2026-09-17, x86_64-elf-gcc 툴체인 전환 중 실측 발견,
    // PN-158B6B2F] `gNormalQueues[]`(NormalQueue, 내부에 자기 자신을
    // 가리키는 `Node _sentinel` 포함)의 정적 생성이 컴파일러에 따라
    // 신뢰할 수 없다는 것을 실측으로 확인했다 - clang은 이 self-참조
    // NSDMI(`prev = this`)를 링크 타임 상수로 해석해 문제없이 동작했지만,
    // GCC는 같은 코드에서 `.bss`에 전부 0으로만 배치하고 `.init_array`
    // 항목도 만들지 않아(둘 다 확인) 실제로는 아무 초기화도 일어나지
    // 않았다 - `Scheduler::runLoop()`이 첫 `pickNext()`를 부르자마자
    // `Node::unlink()`가 널/쓰레기 포인터를 역참조해 크래시했다(GPF).
    // 컴파일러의 정적 초기화 추론에 기대지 않고 부팅 시 명시적으로
    // `init()`을 불러 이 클래스가 원래 `List::init()` 문서 주석이
    // 이미 경고해 둔 "슬랩 재사용 메모리 위에 얹을 때"와 똑같은 상황
    // (실제로는 "정적 배열 생성 자체가 컴파일러별로 신뢰 불가"라는
    // 새로운 이유)에 놓였다고 보고 방어적으로 고친다.
    for (uint32_t i = 0; i < kMaxCores; ++i) {
        gNormalQueues[i].init();
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
    // [신규, 2026-09-18, SP-76250478 §3 항목2, PN-0EB2FABF, RM-48E1E610
    // 그룹0 call9] SelfTerminate와 같은 이유로 BSP에서 한 번만.
    SyscallRegistry::registerHandler(kSyscallEndpointSelfTerminateThread, &gSelfTerminateThreadHandler);
    // [신규, 2026-09-17, SP-B26CDBDD §7, RM-48E1E610 그룹0 call5]
    SyscallRegistry::registerHandler(kSyscallEndpointSetTaskWeight, &gSetTaskWeightHandler);
    // Push/Pull 로드밸런싱(PN-04D6197A, SP-9525C4C0 §4) - idle 코어
    // 기상 IPI. Push 분기(§2)는 아직 미구현(QU-9325BD40 답변 대기)
    // 이라 지금은 이 벡터를 보내는 호출부가 없지만, 벡터 등록 자체는
    // Pull과 무관하게 먼저 완료해 둔다.
    Idt::registerHandler(kLoadBalanceWakeVector, kLoadBalanceWakeIsr);
}

namespace {

// IA32_TSC_AUX(SP-0666DB3C §12.4-1) - RDTSCP가 이 MSR의 값을 ECX로
// 그대로 돌려주므로, 코어별로 자기 논리 인덱스를 한 번 심어 두면
// 이후 매번 메모리 접근 없는 순수 명령어 하나로 O(1) 조회가 된다.
constexpr uint32_t kMsrTscAux = 0xC0000103;

// 기존 O(코어 수) 선형 스캔 - RDTSCP 미지원 CPU의 런타임 폴백이자,
// RDTSCP 지원 CPU에서도 이 코어의 진짜 인덱스를 최초 한 번 계산할
// 때(initCoreIndexForThisCore) 재사용한다.
uint32_t kScanCoreIndexByApicId() {
    const uint32_t apicId = Lapic::id();
    const uint32_t cpuCount = Acpi::cpuCount();
    for (uint32_t i = 0; i < cpuCount; ++i) {
        if (Acpi::cpuApicId(i) == apicId) {
            return i;
        }
    }
    return 0;
}

// 전체 머신에 동일하게 적용되는 CPU 기능이라 코어별로 다시 검사할
// 필요가 없다 - BSP가 가장 먼저 initCoreIndexForThisCore()를 호출할
// 때 한 번 계산해 두면, AP는 순차 기동(PL-65C20380 v1)이라 그 이후에만
// 자기 차례가 오므로 경쟁 없이 그대로 읽기만 한다.
bool gRdtscpChecked = false;
bool gRdtscpSupported = false;

bool kCheckRdtscpSupport() {
    uint32_t eax = 0x80000001, ebx = 0, ecx = 0, edx = 0;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));
    return (edx & (1u << 27)) != 0;
}

}  // namespace

void Scheduler::initCoreIndexForThisCore() {
    if (!gRdtscpChecked) {
        gRdtscpSupported = kCheckRdtscpSupport();
        gRdtscpChecked = true;
    }
    if (!gRdtscpSupported) {
        return;  // 폴백 - currentCoreIndex()가 계속 선형 스캔을 쓴다
    }
    const uint32_t coreIndex = kScanCoreIndexByApicId();
    const uint32_t low = coreIndex;
    const uint32_t high = 0;
    asm volatile("wrmsr" : : "c"(kMsrTscAux), "a"(low), "d"(high));
}

uint32_t Scheduler::currentCoreIndex() {
    if (gRdtscpSupported) {
        uint32_t aux;
        asm volatile("rdtscp" : "=c"(aux) : : "rax", "rdx");
        return aux;
    }
    return kScanCoreIndexByApicId();
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
    //
    // [수정, 2026-09-21, SP-A252E82F 구현 중 실측 확인] 원래 이 임계구역은
    // 무조건 `cli`+`sti` 페어였다 - pci.cpp의 PciConfigAccessGuard가
    // 이미 문서화해 둔 바로 그 위험(§ "무조건 cli+sti 페어는 항상
    // 인터럽트가 켜진 채로 불린다는 전제") 그대로, 이 함수가 이미
    // `cli`된 컨텍스트(인터럽트 핸들러 - 예: 디바이스 IRQ가
    // WaitQueue::wakeOne()/AsyncReactor 완료 경로를 거쳐 이 함수를
    // 부르는 경우, wait_queue.cpp/async_task.cpp 참고)에서 불리면
    // `sti`가 그 핸들러의 IF=0 불변조건을 실수로 깨뜨렸다 - 실측으로
    // 확인(SP-A252E82F가 전제하는 "일반 인터럽트는 절대 중첩되지
    // 않는다"는 이 버그 때문에 실제로는 깨져 있었다: HPET(벡터 0x22)가
    // LAPIC 스케줄러 틱(0x24) 처리 도중 이 함수의 `sti` 창으로
    // 끼어들어 일반 벡터 디스패치 스택 위에서 또 다른 InterruptFrame이
    // 만들어지는 것을 gdb로 직접 확인). PciConfigAccessGuard와 동일한
    // 기법(진입 시점의 실제 RFLAGS를 저장해 뒀다가 그대로 복원)으로
    // 바꿔, 이미 cli된 채로 불려도 그 IF=0을 그대로 지킨다.
    uint64_t savedRflags;
    asm volatile("pushfq; pop %0; cli" : "=r"(savedRflags) : : "memory");
    if (task->inRunQueue) {
        asm volatile("push %0; popfq" : : "r"(savedRflags) : "memory", "cc");
        return;  // 이미 어느 큐에 들어 있다 - 다시 넣으면 이중 스케줄링
    }
    task->inRunQueue = true;
    task->state = TaskState::Ready;
    if (task->taskClass == TaskClass::RealTime) {
        gRtQueues[coreIndex].pushBack(task);
    } else {
        // Push(PN-04D6197A, SP-9525C4C0 §2) - affinity가 걸려 있으면
        // (전체 코어가 아니면) 무조건 원래 coreIndex 그대로, 아니면
        // 이 코어 큐가 임계치(kPushThresholdLength)를 넘었을 때만
        // 가장 한가한 코어로 대신 넣는다. 대상 큐만 바뀔 뿐 "확인+
        // 세팅+push"가 여전히 이 하나의 cli 임계구역 안에서 원자적으로
        // 끝나므로 위 이중 스케줄링 방지 불변조건은 그대로 유지된다 -
        // approxLength() 읽기/kFindLeastLoadedCore의 스캔은 락 없는
        // 원자적 조회뿐이라 이 임계구역 안에서 수행해도 다른 코어의
        // 진행을 막지 않는다.
        uint32_t targetCore = coreIndex;
        if (task->affinityMask == kTaskAffinityAllCores &&
            gNormalQueues[coreIndex].approxLength() > kPushThresholdLength()) {
            const uint32_t candidate = kFindLeastLoadedCoreNumaAware(coreIndex, task->numaNode);
            if (candidate != coreIndex) {
                // [PN-F55FB154] Pull(runLoop)은 이미 kCanMigrateFpuSafely로
                // 이 검사를 하는데 Push만 빠져 있었다 - task가 candidate가
                // 아닌 다른 코어에서 지금 살아있는 FPU 소유자라면(원격
                // FXSAVE 불가, kCanMigrateFpuSafely 문서 주석 참고) 이번
                // Push를 보류하고 targetCore를 coreIndex로 유지한다(Pull의
                // 스킵/재시도 정책과 동일 - 다음 기회에 다시 시도됨).
                const uint32_t fpuOwnerCore = kFindFpuOwnerCore(task);
                if (fpuOwnerCore == gCoreCount || fpuOwnerCore == candidate) {
                    targetCore = candidate;
                }
            }
        }
        // [신규, 2026-09-17, SP-B26CDBDD §2.3] 굶주림 방지 - 이 지점은
        // task->inRunQueue가 false->true로 바뀌는 "새로 큐에 들어가는"
        // 경로 그 자체다(위에서 이미 true였으면 조기 반환했다) - 방금
        // 깨어났거나 막 생성된 Task의 vruntime이 대상 큐의 현재
        // 최솟값보다 작으면(오래 블로킹돼 있었거나 갓 생성돼 0인 경우)
        // 그 최솟값까지만 끌어올린다(CFS 표준 "min_vruntime 보정"의
        // 축소판) - 더 뒤처지게(크게) 만들지는 않는다.
        uint64_t minVruntime;
        if (gNormalQueues[targetCore].minVruntime(&minVruntime) && task->vruntime < minVruntime) {
            task->vruntime = minVruntime;
        }
        gNormalQueues[targetCore].insert(task);
        if (targetCore != coreIndex) {
            kWakeCoreIfIdle(targetCore);
        }
    }
    asm volatile("push %0; popfq" : : "r"(savedRflags) : "memory", "cc");
}

void Scheduler::scheduleImmediate(uint32_t coreIndex, Task* task) {
    if (coreIndex >= gCoreCount) {
        coreIndex = 0;
    }
    // enqueue()와 같은 이유 - 위 주석 참고(SP-A252E82F 구현 중 실측
    // 확인한 무조건 cli+sti 페어의 위험, PciConfigAccessGuard와 동일한
    // RFLAGS 저장/복원 기법으로 교체).
    uint64_t savedRflags;
    asm volatile("pushfq; pop %0; cli" : "=r"(savedRflags) : : "memory");
    if (task->inRunQueue) {
        asm volatile("push %0; popfq" : : "r"(savedRflags) : "memory", "cc");
        return;
    }
    task->inRunQueue = true;
    task->state = TaskState::Ready;
    gImmediateQueues[coreIndex].pushBack(task);
    asm volatile("push %0; popfq" : : "r"(savedRflags) : "memory", "cc");
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
        // [신규, 2026-09-19, SP-6A563A8F §4] CPU 쿼터 스로틀 - 이번
        // 주기 쿼터를 소진한 그룹 소속 Task는 건너뛰고 큐에 도로
        // 넣는다. `OrderedList`는 popMin()만 지원해 "일부만 보고
        // 도로 넣기"를 자연스럽게 못 하므로, 스로틀된 후보를 이
        // 호출 안에서만 사는 임시 배열에 모아 뒀다가 되돌린다 -
        // vruntime은 건드리지 않는다(이미 큐에 있던 값을 그대로
        // 되돌리는 것이지 "새로 큐에 들어가는" 경로가 아니므로
        // SP-B26CDBDD §2.3 굶주림 방지 보정은 다시 적용 안 함).
        constexpr uint32_t kMaxHeldThrottledCandidates = 32;
        Task* held[kMaxHeldThrottledCandidates];
        uint32_t heldCount = 0;
        while ((task = gNormalQueues[coreIndex].popMin()) != nullptr) {
            ResourceGroup* group = kResourceGroupOf(task);
            if (!group || !kCheckAndResetCpuPeriod(group)) {
                break;  // 그룹 없음(비정상, 방어적) 또는 스로틀 아님 - 이 Task를 뽑는다
            }
            if (heldCount < kMaxHeldThrottledCandidates) {
                held[heldCount++] = task;
            }
            task = nullptr;  // 계속 탐색
            if (heldCount == kMaxHeldThrottledCandidates) {
                // 비정상적으로 많은 그룹이 동시 스로틀 - 더 찾지 않고
                // 이번 틱은 그냥 "대신 돌릴 게 없음"으로 취급한다(다음
                // 틱에 재시도). RM-23F4B687 §4 - v1 상한은 넉넉히 잡아
                // 실사용에서 닿지 않게 한다.
                break;
            }
        }
        for (uint32_t i = 0; i < heldCount; ++i) {
            gNormalQueues[coreIndex].insert(held[i]);
        }
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

void Scheduler::onTick(InterruptFrame* frame) {
    // 가장 먼저 EOI - 이 아래서 Task 전환이 일어나면 이 함수 호출은
    // 그 Task가 다시 스케줄될 때까지 "반환"하지 않는다(kContextSwitch
    // 가 콜스택 깊숙이 매달린 채로 남는다) - EOI를 미루면 그 사이
    // 이 코어에 다음 스케줄러 틱이 아예 전달되지 않는다.
    Lapic::sendEoi();

    const uint32_t coreIndex = currentCoreIndex();

    // [PN-F443FE73, SP-677210E6 "워치독"] 이 코어가 살아서 자기
    // 틱조차 못 도는 상황을 겪지 않았다는 증거 - 선점 금지/idle
    // 여부와 무관하게 항상 증가시켜야 워치독 판정이 정확하다(위
    // Timer::onTick()과 같은 이유로 어떤 조기 반환보다도 먼저).
    gHeartbeat[coreIndex].fetchAdd(1);

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

    // [PN-F443FE73, SP-677210E6 "워치독"] BSP 자신의 틱 핸들러가
    // 감시자 역할을 겸한다 - 100틱(약 1초)마다 자신을 제외한 온라인
    // 코어 전부의 하트비트가 지난 체크 이후 늘었는지 확인한다. 이
    // 블록도(선점 금지/idle 여부와 무관하게) Timer::onTick()과 같은
    // 이유로 어떤 조기 반환보다 먼저 있어야 한다 - 그래야 이 코어
    // 자신이 다른 이유로 일찍 반환해도 워치독 주기 자체는 어긋나지
    // 않는다.
    if (coreIndex == gBspCoreIndex) {
        ++gWatchdogTickCounter;
        if (gWatchdogTickCounter >= kWatchdogCheckIntervalTicks) {
            gWatchdogTickCounter = 0;
            const uint32_t cpuCount = Acpi::cpuCount();
            for (uint32_t i = 0; i < cpuCount; ++i) {
                // [수정, 2026-09-17, PN-907C5289 실측 확인] 아직 SIPI
                // 트램폴린 도중(유효한 IDT 없음)이라 gHeartbeat[i]가
                // 한 번도 안 늘어난 코어를 "먹통"으로 오판해 NMI를
                // 보내면 그 코어가 트리플 폴트를 일으킨다 - 기동을
                // 아직 안 마친 코어는 애초에 감시 대상이 아니므로
                // Smp::isCoreOnline()으로 걸러낸다(Nmi::stopAllOtherCores()
                // 에 적용한 것과 동일한 근거/수정).
                if (i == gBspCoreIndex || gWatchdogTriggered[i] || !Smp::isCoreOnline(i)) {
                    continue;
                }
                const uint32_t current = gHeartbeat[i].load();
                if (current == gHeartbeatLastSeen[i]) {
                    // 지난 체크 이후 이 코어가 자기 스케줄러 틱을 단
                    // 한 번도 못 돌았다 - 멈춘 것으로 판단.
                    gWatchdogTriggered[i] = true;
                    Nmi::send(i, NmiReason::WatchdogTrap);
                } else {
                    gHeartbeatLastSeen[i] = current;
                }
            }
        }
    }

    if (gPreemptDisableCount[coreIndex] > 0) {
        return;  // 선점 금지 구간 - 인터럽트 자체는 처리됐으니 그냥 계속 실행
    }

    // [신규, 2026-09-20, PN-584DB994/DC-06FC78E8] 이 지점부터 아래
    // `kContextSwitchFromISR()` 호출 직전까지 `gCurrentTask[coreIndex]`
    // 등 전역 디스패치 상태를 만진다 - 이 창에서 같은 코어에 또 다른
    // 인터럽트가 도착해 `onTick()`/`onForcedMigration()`이 재진입되면
    // 그 상태가 서로 덮어써진다(실측으로 확인된 근본 원인, 위
    // `gInDispatchWindow` 선언부 문서 주석 참고). 이미 누군가(중첩된
    // 바깥쪽 호출) 이 창 안에 있으면 이번 인터럽트의 스케줄링 결정은
    // 포기한다 - EOI는 이미 보냈으니 인터럽트 자체는 정상 처리된
    // 것으로 취급.
    DispatchWindowGuard dispatchGuard(coreIndex);
    if (!dispatchGuard.acquired()) {
        return;
    }

    Task* current;
    {
        RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
        current = gCurrentTask[coreIndex];
    }
    if (!current) {
        // [갱신, 2026-09-19, PN-D47FBB8D] 이제 이 분기는 오직 부팅
        // 극초반(startTickOnThisCore() 호출 이후, 이 코어의
        // enterIdleLoop()이 아직 gIdleTask[coreIndex]를 구성하기 전)의
        // 좁은 창에서만 도달한다 - 그 창을 지나면 gCurrentTask는 항상
        // 실제 Task 아니면 &gIdleTask[coreIndex]를 가리키므로 다시는
        // 여기 오지 않는다. 방어적으로만 남겨 둔다.
        return;
    }

    Task* next = pickNext(coreIndex);
    // [신규, 2026-09-18, PN-44C91D6E - 정정, 2026-09-19, PN-414BF822,
    // 설계자 답변(QU-CC3BB5AE)] 예전엔 `next`가 한 번도 디스패치된 적
    // 없으면(`hasEverRun==false`) 이 자리(onTick()의 Task-to-Task 직접
    // 전환, 원래 타이머 인터럽트 컨텍스트에 중첩된 채로 진행됨)에서
    // 첫 디스패치 자체를 미뤘다(`kContextSwitch`의 `popfq`가 RFLAGS의
    // IF=1을 인터럽트-복귀 이전에 CPU에 반영해 재중첩 위험이 있다는
    // 이유, devmgr+dbgtarget SpawnProcess 연속 호출 하네스로 실측
    // 확인됨) - 그런데 이 "미룸"이 "현재 실행 중인 Task가 절대
    // 자발적으로 CPU를 내려놓지 않으면(예: `for(;;) { pause; }`), 이
    // 코어가 다시는 idle로 안 돌아가 이 Task가 영원히 첫 디스패치를
    // 못 받는" 라이브락을 낳았다(devmgr/fs/pubreg가 전혀 실행되지
    // 않던 결함, PN-414BF822 실측 확인 - `kSpawnInitProcess()`가
    // 스폰한 `init`이 무한 대기 루프에 들어간 뒤로는 다시는 idle로
    // 안 돌아가 `kSpawnServiceProcesses()`가 스폰한 서비스들이 전부
    // 대기만 함). 이제는 미루지 않는다 - [갱신, 2026-09-20, PN-81E49523
    // 2단계] 당시엔 아래 전환 지점이 `next->hasEverRun` 값으로 두
    // 메커니즘(`kContextSwitch`/`kContextSwitchToFreshTask`) 중 골랐지만,
    // 그 구분 자체가 이제 `kContextSwitchFromISR` 하나로 통일되며
    // 사라졌다 - 아래 그 함수 호출부 주석 참고.

    // Zombie면(PN-71C3D483 - kTaskOnFallingToEnd가 self-terminate 제출
    // 직전 스스로 표시해 둔 상태) 재삽입하지 않는다 - 곧 리액터의
    // SelfTerminateHandler가 Scheduler::retireTask(current)로 정리
    // 큐에 등록한다. 그 전까지 이 Task는 어느 큐에도 없는 채로 그냥
    // "스위칭되어 나간" 상태로만 남는다(다시 뽑힐 걱정 없음).
    //
    // [신규, 2026-09-17, SP-245D130B §4] ResourceGroup freeze - 이
    // 재스케줄 결정 지점이 바로 resource_group.h의 kCheckAndMarkFrozen()
    // 문서 주석이 가리키는 "그 지점"이다. Zombie와 같은 급의 "재삽입
    // 안 함" 분기 하나를 더 두되, 이쪽은 대신 Blocked로 남겨 나중에
    // ResourceGroup::thaw()가 다시 enqueue()하게 한다(Zombie는 영원히
    // 안 돌아오지만 이쪽은 그룹이 풀리면 돌아온다는 차이).
    //
    // [신규, 2026-09-17, SP-9A6D579F §3.5/§4] 디버그 정지도 같은 지점
    // 에서 같이 확인한다 - #DB 콜백(kHandleUserBreakpointHit,
    // debug_session.cpp)이 `pausedByDebugger`를 세워 뒀으면
    // `kIsPausedByDebugger()`가 그걸 읽어 이 재삽입 결정에 반영한다
    // (그 콜백 자신은 이 Task를 당장 멈추지 못한다는 한계가 있어 -
    // 그 문서 주석 참고 - 이 지점이 실제로 멈추는 유일한 장소다).
    // 두 사유 중 하나라도 있으면 Blocked - `||` 대신 두 함수를 각각
    // 부르는 이유는 `kCheckAndMarkFrozen()`이 부수효과(frozenByGroup
    // 세팅)가 있어 단락 평가로 건너뛰면 안 되기 때문.
    //
    // [정정, 2026-09-19, PN-D47FBB8D - 실측으로 발견한 두 번째 버그를
    // 반영해 재구성] 이 검사(`kCheckAndMarkFrozen`/`kIsPausedByDebugger`)
    // 는 **`next`의 존재 여부와 무관하게** 항상 평가해야 한다(원래
    // 발견된 근본 버그) - 하지만 "평가한다"가 "매번 idle로 실제 전환한다"
    // 는 뜻은 아니다. 처음엔 `next`가 없을 때 무조건 `gIdleTask`로
    // 폴백해 아래 공용 전환 코드로 흘려보냈는데, 그러면 current가
    // 멀쩡한(freeze/디버그 정지 어느 쪽도 아닌) 경우에도 매 틱마다
    // `enqueue(coreIndex, current)` 후 idle로 전환하게 되어 - 이
    // Task가 큐에 들어가 있는(=다른 코어의 Pull 로드밸런싱이 훔쳐갈 수
    // 있는) 시점과 이 코어 자신이 실제로 `kContextSwitch`로
    // `current->savedRsp`를 확정 짓는 시점 사이에 새로운 경쟁 창을
    // 열어 버렸다(실측 재현: SMP=4에서 #DB 폭주/Page Fault/Unrouted
    // interrupt로 크래시 - 다른 코어가 아직 안 끝난 전환 중인 Task를
    // 훔쳐가 낡은 savedRsp로 재개하며 스택이 깨짐). **이 경쟁은 매
    // 틱(next가 없을 때마다) 열렸었다 - 원래는 진짜 두 실제 Task가
    // 같은 코어를 경쟁할 때만(드묾) 열리던 것과 달리, 이 폴백 자체가
    // 그 경쟁을 상시화했다.** 수정: current가 멀쩡하고 `next`도 없으면
    // (Zombie도, frozen/디버그 정지도 아님) **enqueue도 전환도 전혀
    // 하지 않고 그냥 return**한다(원래 "next==null이면 return"과
    // 동일 - 불필요한 전환 방지 최적화를 그대로 되살림). idle로의
    // 실제 전환은 current가 **정말로 이 코어를 떠나야 하는**
    // 경우(Zombie 강제 퇴거, 또는 방금 Blocked로 전환됨)에만
    // 일어난다 - 이 두 경우는 current가 어느 큐에도 들어가지 않으므로
    // (Zombie/Blocked는 enqueue 대상이 아님) 위와 같은 훔쳐가기 경쟁
    // 자체가 성립하지 않는다.
    if (current->state == TaskState::Zombie) {
        // mustLeaveCore(Zombie) - 아래 `if (!next)` 폴백이 그대로 처리.
    } else {
        const bool frozenByGroup = kCheckAndMarkFrozen(current);
        const bool pausedByDebugger = kIsPausedByDebugger(current);
        // [신규, 2026-09-19, PN-0AC554C2 1단계, QU-25E1C297 답변] 세
        // 번째 조건 - `current->blockedOn`(이제 리스트)이 해소되지 않은
        // Waitable을 여전히 담고 있으면 이 Task는 Blocked여야 한다.
        // `kCheckAndMarkFrozen()`과 동일한 이유로 `||` 단락 평가로
        // 건너뛰면 안 된다(이 함수 자신이 완료된 엔트리를 지우는 부수
        // 효과를 갖는다) - 매번 반드시 호출한다. 오늘 기준 이 리스트를
        // 채우는 유일한 경로(WaitQueue)는 채워져 있는 동안 이 Task가
        // 이미 파킹돼 있어(Scheduler::parkCurrent()로 직접) onTick의
        // "current" 후보가 될 수 없으므로, 이 조건은 아직 실제로
        // true가 될 기회가 없다 - 순수 추가, 관찰 가능한 동작 변화 없음.
        const bool blockedOnPending = kDrainAndCheckBlockedOn(current);
        if (frozenByGroup || pausedByDebugger || blockedOnPending) {
            current->state = TaskState::Blocked;
            // [신규, 2026-09-17, SP-9A6D579F §3.5, DC-47000304 (A) 채택]
            // 지금 이 지점에서 손에 쥔 frame(이 Task 자신의 커널 스택
            // 위, isr_common_stub이 쌓아 둔 자리)이 DebugGetRegisters/
            // SetRegisters가 다룰 진짜 대상 - 이 Task가 재개되기
            // 전까지 아무도 이 스택을 건드리지 않으므로 그 자리에
            // 그대로 살아있다(debug_session.h kSaveDebugRegistersSnapshot
            // 문서 주석 참고). pausedByDebugger가 아니면(그룹 freeze만)
            // 이 함수 내부에서 조용히 아무 일도 안 한다.
            if (pausedByDebugger) {
                kSaveDebugRegistersSnapshot(current, frame);
            }
            // mustLeaveCore(방금 Blocked) - 아래 `if (!next)` 폴백이 처리.
        } else if (next) {
            // [신규, 2026-09-17, SP-B26CDBDD §3.2/§5, PN-158B6B2F] vruntime/
            // cpuTicksUsed 갱신 + ResourceGroup CPU 계정 - 반드시 enqueue()
            // (그 vruntime을 정렬 키로 삽입 위치를 정한다) 호출보다 먼저다.
            // isUserLevel 조건은 커널 자신의 Normal Task(있다면)까지 공정
            // 스케줄링/계정 대상으로 끌어들이지 않기 위함(SP-B26CDBDD §3.2 -
            // 실제로 지금은 이 코드 경로에 도달하는 Normal Task가 전부
            // UserThread뿐이라 이 조건이 당장 무언가를 걸러내진 않지만,
            // 설계가 명시한 조건이라 그대로 반영한다).
            if (current->taskClass == TaskClass::Normal && current->isUserLevel) {
                current->vruntime += (kEffectiveWeightBase * kVruntimeScale) / kEffectiveWeightOf(current->weight);
                current->cpuTicksUsed += 1;
                if (SharedPtr<Process> proc = static_cast<UserThread*>(current)->process.lock()) {
                    if (ResourceGroup* group = proc->group) {
                        group->accounting.totalCpuTicks += 1;  // 쿼터 없어도 항상 집계(SP-245D130B §5)
                        if (group->cpu.periodTicks != 0) {      // 쿼터 활성 그룹만
                            // [신규, 2026-09-19, SP-6A563A8F §3] 증가 전에
                            // 먼저 주기 롤오버를 확인한다 - 안 그러면
                            // usedTicksInPeriod가 주기 경계 없이 무한정
                            // 누적된다(pickNext()의 스로틀 판정 쪽만
                            // 롤오버를 확인하는 것으로는 불충분 - 이
                            // 그룹이 한동안 pickNext()에서 안 뽑히면
                            // 여기서만 계속 늘어남).
                            kCheckAndResetCpuPeriod(group);
                            group->cpu.usedTicksInPeriod += 1;
                        }
                    }
                }
            }
            // current가 idle 자신이면(다른 실제 Task로 전환하며 idle을
            // 내보내는 경우) 절대 enqueue()하지 않는다 - task.h
            // TaskClass::Idle 문서 주석 참고(vruntime=0 고정이라
            // popMin()이 매번 idle만 최우선으로 뽑아 실제 작업을 굶길
            // 수 있음).
            if (current != &gIdleTask[coreIndex]) {
                enqueue(coreIndex, current);  // 라운드로빈(vruntime 정렬) - Ready로 재삽입
            }
        } else {
            // current 멀쩡함 + 대신 돌릴 실제 Task도 없음 - 위 문서
            // 주석대로 아무 것도 안 하고 그냥 계속 실행한다(경쟁 창을
            // 열지 않기 위해 idle로 억지 전환하지 않음).
            return;
        }
    }
    if (!next) {
        // mustLeaveCore인데 next가 없는 경우(Zombie 강제 퇴거, 또는
        // 방금 Blocked됨) - idle로 폴백한다. current는 이미 어느
        // 큐에도 없는 상태(Zombie/Blocked)라 이 폴백 자체는 위에서
        // 우려한 "훔쳐가기 경쟁"을 열지 않는다.
        next = &gIdleTask[coreIndex];
    }
    if (next == current) {
        // 이론상 도달 불가 - `mustLeaveCore`는 idle 자신에 대해서는
        // 절대 true가 될 수 없고(kCheckAndMarkFrozen/kIsPausedByDebugger
        // 둘 다 isUserLevel==false인 idle엔 항상 false), current가
        // 멀쩡한데 next가 없는 경우는 바로 위에서 이미 return해 이
        // 지점 자체에 도달하지 않는다 - 방어적으로만 남겨 둔다.
        return;
    }
    {
        RwSpinlockWriteGuard guard(gCurrentTaskLock[coreIndex]);
        gCurrentTask[coreIndex] = next;
    }
    next->state = TaskState::Running;
    kSyncRsp0ForDispatch(next);
    kSyncCr3(next);
    kSyncFpu(next, coreIndex);
    kSyncDebugRegs(next);
    kSyncFsBase(next);
    // [갱신, 2026-09-20, PN-81E49523 2단계, 설계자 지시("컨텍스트 스위치
    // 자체를 kContextSwitchFromISR과 kContextSwitch 둘로 나눠 구현")]
    // 이 함수는 인터럽트 핸들러(타이머 ISR) 내부에서 실행 중이므로,
    // `frame`(하드웨어+isr_common_stub이 이미 만들어 둔 진짜
    // InterruptFrame)을 그대로 current의 TaskTcb로 캡처하는
    // `kContextSwitchFromISR`을 쓴다 - current의 커널 스택(지금 이
    // 인터럽트 프레임이 쌓여 있는 바로 그 스택) 위에서 호출 중이므로,
    // 나중에 current가 다시 선택되면 바로 이 `frame` 그대로 iretq되어
    // 재개된다. next가 이번이 첫 디스패치인지 여부와 무관하게 항상
    // 이 함수 하나로 통일된다 - next->tcb는 이미 한 번이라도
    // 디스패치된 적 있는 Task라면 실제 캡처된 TaskTcb를, 한 번도 없는
    // Task라면 Task::init()이 지어 둔 가짜 TaskTcb를 가리키며, 어느
    // 쪽이든 모양이 완전히 같아(TaskTcb=InterruptFrame) 이 함수 하나로
    // 균일하게 착지한다(예전 PN-414BF822의 `kContextSwitchToFreshTask`/
    // `hasEverRun` 분기는 이 통일로 더 이상 필요 없어져 제거됨 - 그
    // 분기가 있었던 근본 이유(popfq+ret의 RFLAGS 타이밍 위험)가 iretq
    // 기반 착지로 애초에 사라졌기 때문).
    static_assert(kGdtKernelCodeSelector == 0x08 && kGdtKernelDataSelector == 0x10,
                  "gdt.h 값이 바뀌면 context_switch.S/task.cpp/async_task.cpp의 리터럴도 같이 바꿀 것");
    // [수정, 2026-09-20, PN-81E49523 2단계 - minicore-3c 교차 진단으로
    // 발견] 바로 위 문단이 설명하는 실제 전환 호출 자체가 누락돼
    // 있었다 - kSyncCr3(next) 등으로 next의 디스패치 상태만 준비해
    // 두고 실제 레지스터 저장/복원(iretq 착지)을 한 번도 안 한 채
    // 함수가 그대로 끝나 버려서, current가 다시 원래 인터럽트 프레임
    // 그대로(그러나 CR3는 이미 next로 바뀐 채) 재개되던 것이 크래시의
    // 근본 원인이었다(cr2≈0x8 - kContextSwitchFromISR을 호출한 적이
    // 없으니 current->tcb는 여전히 이전 상태 그대로였고, 그 다음
    // 인터럽트가 그 어긋난 CR3 위에서 또 전환을 시도하며 실제로
    // null에 가까운 tcb를 통해 쓰기가 일어난 것으로 보인다).
    // [신규, 2026-09-20, PN-584DB994/DC-06FC78E8] 실제 전환은 바로 다음
    // 줄에서 일어난다 - 그 순간부터 이 코어는 진짜로 다른 Task/스택
    // 위에서 실행되므로, 재진입 보호를 여기서 명시적으로 내려 다음
    // 인터럽트(다른 Task를 위한 정상적인 onTick())가 막히지 않게 한다.
    dispatchGuard.release();
    kContextSwitchFromISR(&current->tcb, next->tcb, frame);
    // [제거, 2026-09-20, PN-81E49523 2단계] 예전엔 여기 `kSyncCr3(current)`
    // 호출이 있었다(PN-B5FD7B75가 발견한 "네 번째 CR3 재동기화 지점"
    // 수정) - 그 근거는 "current가 나중에 다시 선택되면 실행이 정확히
    // 여기(바로 위 호출 다음)로 돌아온다"는 전제였는데, `kContextSwitchFromISR`
    // 도입으로 그 전제가 깨졌다: current의 캡처가 이제 `frame`(진짜
    // 원래 인터럽트 지점)이라, current가 재개되면 이 자리로 전혀
    // 돌아오지 않고 곧장 그 원래 지점으로 iretq된다 - 이 줄은 이제
    // 절대 도달하지 않는 죽은 코드였다. **수정된 진짜 불변조건**: CR3
    // 재동기화 책임은 항상 "next를 실제로 디스패치하는 쪽"이 진다 -
    // 이 함수는 이미 위에서 `kSyncCr3(next)`를 부르므로 next 쪽은
    // 문제없다. current가 나중에 (다른 코어의 onTick()/이 코어의
    // runLoop() idle→Task 경로 등) 어딘가에서 next 취급을 받아 다시
    // 뽑힐 때, **그 시점의 디스패처가 다시 `kSyncCr3(next)`를 불러야
    // 한다** - `runLoop()`의 idle→Task 디스패치도 이제 이 규칙을
    // 예외 없이 따르도록 맞췄다(그 함수 문서 주석 참고, 예전엔 "재개
    // 지점이 스스로 동기화한다"고 믿고 생략했으나 그 신뢰가 더 이상
    // 성립하지 않음).
}

void Scheduler::requestForcedMigration(uint32_t fromCore, uint32_t targetCore) {
    // tlb_shootdown.cpp와 동일한 이유로 요청 슬롯 채우기 자체는
    // 직렬화가 필요하다(PN-D132A1E9 경고 동일 적용) - v1은 호출부가
    // 하나뿐인 수동/진단 API라 별도 락 없이 그대로 채운다.
    {
        RwSpinlockReadGuard guard(gCurrentTaskLock[fromCore]);
        gForcedMigrationRequest.target = gCurrentTask[fromCore];  // 요청 시점
    }
    // 스냅샷 - IPI 도착 시점에 이미 다른 Task로 바뀌어 있을 수 있다
    // (onForcedMigration()의 current != target 방어가 이 경쟁을 무해하게
    // 처리한다).
    gForcedMigrationRequest.targetCore = targetCore;
    Lapic::sendFixedIpi(Acpi::cpuApicId(fromCore), static_cast<uint8_t>(kForcedMigrationVector));
}

void Scheduler::onForcedMigration(InterruptFrame* frame) {
    // 가장 먼저 EOI - onTick()과 정확히 같은 이유(위 kForcedMigrationVector
    // 선언부 주석과 onTick() 자신의 주석 참고). 이 아래서 kContextSwitch로
    // 다른 Task의 스택으로 전환하면 이 함수 호출은 그 Task가 다시
    // 스케줄될 때까지 "반환"하지 않는다 - EOI를 미루면 그 사이 이
    // 코어에 어떤 인터럽트도(다음 스케줄러 틱 포함) 전달되지 않는다.
    Lapic::sendEoi();

    const uint32_t coreIndex = currentCoreIndex();

    // [신규, 2026-09-20, PN-584DB994/DC-06FC78E8] onTick()과 동일한
    // 재진입 보호 - 아래 `gCurrentTask[coreIndex]` 갱신부터
    // `kContextSwitchFromISR()` 호출 직전까지 이 코어에 다른 인터럽트가
    // 겹치면(예: onTick()이 동시에 재진입) 전역 디스패치 상태가
    // 덮어써진다(위 `gInDispatchWindow` 선언부 문서 주석 참고). 이미
    // 다른 호출(onTick() 자신 포함)이 이 창 안에 있으면 이번 IPI의
    // 이관 시도는 포기한다 - `requestForcedMigration()`이 이미 "가능
    // 하면 지금 옮긴다"는 근사적 요청으로 설계돼 있어(위 문서 주석),
    // 포기해도 안전하다.
    DispatchWindowGuard dispatchGuard(coreIndex);
    if (!dispatchGuard.acquired()) {
        return;
    }

    Task* current;
    {
        RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
        current = gCurrentTask[coreIndex];
    }

    // 경쟁 방어: IPI가 도착했을 때 이미 이 코어가 idle이거나(current==
    // nullptr) 요청 시점과 다른 Task를 실행 중이면(그 사이 이 Task가
    // 스스로 끝났거나 블로킹돼 자연스럽게 전환됐을 수 있음) 이 요청은
    // 그냥 무해하게 버린다 - 목표는 "가능하면 지금 옮긴다"지 "반드시
    // 옮긴다"가 아니다(근사적 로드밸런싱, approxLength()와 같은 정신).
    if (!current || current != gForcedMigrationRequest.target) {
        return;
    }
    const uint32_t targetCore = gForcedMigrationRequest.targetCore;

    Task* next = pickNext(coreIndex);

    // FPU 강제 반납(§4) - kContextSwitch 전에 반드시 먼저.
    kEvictFpuBeforeMigration(current, coreIndex);

    // [수정, 2026-09-21, PN-6CE4DD35] onTick()과 동일하게
    // kCheckAndMarkFrozen()/kIsPausedByDebugger()를 확인한다 - 둘 중
    // 하나라도 참이면 재삽입(enqueue) 대신 Blocked로 남긴다(재삽입하면
    // 그룹 freeze/디버그 정지 중인 Task를 실수로 재개시킬 수 있음).
    // `kDrainAndCheckBlockedOn()`은 onTick()에만 있고 여기엔 추가하지
    // 않는다 - 이 함수(강제 이관)의 대상은 항상 "지금 이 코어에서
    // 실행 중이던 current"이므로 이미 WaitQueue에 파킹돼 blockedOn이
    // 채워져 있을 수 없다(onTick() 쪽 문서 주석과 동일한 전제).
    if (current->state != TaskState::Zombie && current != &gIdleTask[coreIndex]) {
        const bool frozenByGroup = kCheckAndMarkFrozen(current);
        const bool pausedByDebugger = kIsPausedByDebugger(current);
        if (frozenByGroup || pausedByDebugger) {
            current->state = TaskState::Blocked;
            if (pausedByDebugger) {
                kSaveDebugRegistersSnapshot(current, frame);
            }
        } else {
            enqueue(targetCore, current);  // <- onTick()과 유일하게 다른 한
                                            // 줄: 같은 코어가 아니라 targetCore에
                                            // 재삽입. [갱신, 2026-09-19,
                                            // PN-D47FBB8D] idle 자신은(이론상
                                            // 이 API의 대상이 될 일이 거의
                                            // 없지만) 큐에 절대 안 들어가야
                                            // 하므로 onTick()과 동일하게 제외.
        }
    }
    // [갱신, 2026-09-19, PN-D47FBB8D] onTick()과 동일한 idle 폴백 -
    // 자세한 이유는 그쪽 문서 주석 참고.
    if (!next) {
        next = &gIdleTask[coreIndex];
    }
    if (next == current) {
        // current 자신이 이미 idle이었고 대신 돌릴 다른 Task도 없다.
        return;
    }
    {
        RwSpinlockWriteGuard guard(gCurrentTaskLock[coreIndex]);
        gCurrentTask[coreIndex] = next;
    }
    next->state = TaskState::Running;
    kSyncRsp0ForDispatch(next);
    kSyncCr3(next);
    kSyncFpu(next, coreIndex);
    kSyncDebugRegs(next);
    kSyncFsBase(next);
    // [갱신, 2026-09-20, PN-81E49523 2단계, 설계자 지시] onTick()과
    // 정확히 같은 이유로 `kContextSwitchFromISR`로 통일 - `frame`(이
    // IPI 핸들러 자신의 InterruptFrame)을 그대로 current의 TaskTcb로
    // 캡처한다. onTick() 위쪽의 상세 주석 참고.
    // [신규, 2026-09-20, PN-584DB994/DC-06FC78E8] onTick()과 동일한
    // 이유로 실제 전환 직전에 재진입 보호를 내린다.
    dispatchGuard.release();
    kContextSwitchFromISR(&current->tcb, next->tcb, frame);
    // [제거, 2026-09-20, PN-81E49523 2단계] onTick()과 정확히 같은
    // 이유로 여기 있던 `kSyncCr3(current)`를 제거했다 - 그 함수의
    // 해당 주석 참고(이제 절대 도달하지 않는 죽은 코드였음).
}

// [신규, 2026-09-17, PN-2008220B] kTaskStartTrampoline과 동일한
// "가짜 kContextSwitch 프레임 위장" 기법(context_switch.S 참고) -
// CR3 동기화만 없을 뿐 레이아웃은 완전히 같다.
extern "C" void kIdleLoopTrampoline();

void Scheduler::enterIdleLoop() {
    const uint32_t coreIndex = currentCoreIndex();

    // [중요, Task::init()과의 차이] RFLAGS를 0x202(IF=1) 같은 고정값
    // 으로 하드코딩하지 않는다 - `Task::init()`은 "새로 태어나는
    // 독립된 실행 흐름"이라 IF=1로 시작하는 게 정책적으로 항상
    // 맞지만, 이 함수는 그게 아니라 **호출자(kMain/kApMain)의 부팅
    // 흐름을 다른 스택 위로 그대로 이어가는 것**이다 - 평범한 C++
    // 함수 호출이었다면 RFLAGS는 호출 전후로 전혀 안 바뀌었을 것.
    // BSP는 이 호출 전에 이미 `sti`를 해 둔 상태(IF=1)로, AP는
    // 아직 `sti` 전(IF=0, ap_trampoline.S 관례 - smp.cpp의 이 함수
    // 호출부 문서 주석 참고)으로 도착한다 - 이 둘을 실수로
    // 하드코딩된 값으로 덮어쓰면 AP가 zombie 정리/pickNext/
    // drainOnce()를 실행하는 동안 인터럽트가 계획보다 일찍 켜지는
    // (또는 BSP가 반대로 꺼지는) 회귀가 생긴다 - 그래서 지금 이
    // 순간의 실제 RFLAGS를 그대로 읽어 프레임에 실어 되돌려 준다.
    uint64_t currentRflags;
    asm volatile("pushfq; pop %0" : "=r"(currentRflags));

    uint8_t* stackTop = gIdleStack[coreIndex] + sizeof(gIdleStack[coreIndex]);
    // [갱신, 2026-09-20, PN-81E49523 2단계, 설계자 지시] `gIdleTaskTcb[coreIndex]`
    // (위 선언부 참고 - 부팅 극초반 제약으로 Slab이 아니라 정적 배열)를
    // 필드별로 직접 채운다. rbx에 &runLoop을 실어 kIdleLoopTrampoline이
    // 그대로 call한다. runLoop()은 인자를 받지 않으므로 r12(트램폴린이
    // rdi로 옮기는 kTaskStartTrampoline과 달리 이 트램폴린은 그 mov도
    // 안 함)를 비롯한 나머지 GPR은 그냥 0으로 채운다. rspOld=stackTop
    // (이 코어의 idle 전용 커널 스택 top 그대로).
    TaskTcb* tcb = &gIdleTaskTcb[coreIndex];
    *tcb = TaskTcb{};
    tcb->rbx = reinterpret_cast<uint64_t>(&runLoop);            // 트램폴린이 call
    tcb->rip = reinterpret_cast<uint64_t>(&kIdleLoopTrampoline);
    tcb->cs = 0x08;                                             // kGdtKernelCodeSelector
    tcb->rflags = currentRflags;                                // 호출 시점 그대로 보존
    tcb->rspOld = reinterpret_cast<uint64_t>(stackTop);
    tcb->ssOld = 0x10;                                          // kGdtKernelDataSelector

    // [신규, 2026-09-19, PN-D47FBB8D] 이 코어의 idle/리액터를
    // `gIdleTask[coreIndex]`(진짜 `Task`)로 등록한다 - 일반
    // `Task::init()`을 쓰지 않는 이유는 위 gIdleTask 선언부 문서
    // 주석 그대로(부팅 극초반 Page/Slab 할당자 의존 회피, RFLAGS
    // 보존 등 이 함수 고유의 제약과 안 맞음) - 방금 채운 고정 TaskTcb
    // 블록을 그대로 이 Task의 tcb로 삼는다.
    gIdleTask[coreIndex].tcb = tcb;
    gIdleTask[coreIndex].taskClass = TaskClass::Idle;
    gIdleTask[coreIndex].state = TaskState::Running;
    // [신규, 2026-09-21, PN-1DFCB337 실측 확정] `gCurrentTask[coreIndex]`를
    // 여기서 세팅하는 순간부터 `kContextSwitch()`가 실제로 이 스택을
    // 떠나기 전까지, CPU는 여전히 지금 이 함수가 서 있는 **부팅
    // 스택**(boot.S의 `boot_stack_bottom` 근방, 영구 페이지테이블
    // 완성 후엔 더 이상 매핑되지 않음) 위에서 실행 중이다 - 이 좁은
    // 창에 스케줄러 틱이 끼어들면(BSP는 이 함수 진입 전에 이미 sti된
    // 상태라 실제로 가능) `onTick()`이 `current==&gIdleTask[coreIndex]`
    // 로 보고 어떤 실제 Task로 전환을 시도하며, 그 순간의 진짜
    // `frame`(부팅 스택 위 주소)을 `gIdleTask[coreIndex].tcb`로
    // 캡처하려다 이미 매핑 해제된 부팅 스택 주소를 읽어 즉시 #PF가
    // 난다 - `runLoop()`이 자기 자신의 동일 지점(줄 2098 부근)에
    // 이미 `cli`로 막아 둔 것과 똑같은 레이스인데, 이 함수만 그
    // 보호가 빠져 있었다(실측: rdx=frame이 정확히
    // `boot_stack_bottom`의 물리주소로 재현됨, PN-1DFCB337 갱신35
    // 참고). `kContextSwitch()`가 도착하는 `gIdleTask[coreIndex].tcb`
    // 자신의 `rflags`는 이미 위에서 `currentRflags`(이 cli 이전
    // 시점의 진짜 IF)로 채워 놨으므로, 여기 새로 추가한 `cli`는
    // 이 Task가 나중에 iretq로 착지할 때의 IF 값에 전혀 영향을 주지
    // 않는다 - 오직 "이 좁은 창 동안만" 인터럽트를 막는다(별도 `sti`가
    // 필요 없음 - `kContextSwitch()`가 절대 반환하지 않고 그대로
    // idle 자신의 착지 지점으로 넘어가기 때문).
    asm volatile("cli");
    {
        RwSpinlockWriteGuard guard(gCurrentTaskLock[coreIndex]);
        gCurrentTask[coreIndex] = &gIdleTask[coreIndex];
    }

    // 지금 서 있는 스택(BSP의 kMain()/AP의 kApMain()이 쓰던 부팅
    // 스택)은 다시는 돌아오지 않으므로 그 캡처 결과가 진짜로 필요하지는
    // 않지만, kContextSwitch의 저장 절반이 *oldTcbSlot이 가리키는
    // 자리에 실제로 값을 써야 하므로(더 이상 push가 스택에 알아서
    // 쌓아 주지 않음 - PN-81E49523 2단계) 유효한 스크래치를 가리키게
    // 한다.
    TaskTcb* discardedOldTcb = &gDiscardedBootTcb[coreIndex];
    kContextSwitch(&discardedOldTcb, gIdleTask[coreIndex].tcb);
    // runLoop()은 [[noreturn]]이라 여기로 절대 돌아오지 않는다 -
    // 컴파일러에게도 그렇게 알려 둔다(이 함수 자신도 [[noreturn]]).
    __builtin_unreachable();
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
            // 리액터가 idle을 흡수한다(SP-F682B889 §3.4/§4, 2026-09-16
            // 재구조 - QU-96BBB769/QU-4034561A/QU-3BDEE348 답변,
            // PN-FEAAF154) - 실행할 Task가 없을 때 곧장 hlt하지 않고
            // 먼저 이 코어의 비동기 실행 큐/지연 타이머를 확인한다.
            // 할 일을 했으면(true) 다음 루프에서 pickNext()부터 다시
            // 확인하고, 정말 아무 것도 없을 때만(false) hlt한다 - hlt는
            // 어떤 인터럽트로도 즉시 깨어나므로(스케줄러 틱 100Hz 포함)
            // 그 자체로 안전하다는 게 이번 재설계의 핵심 전제다.
            if (AsyncReactor::drainOnce(coreIndex)) {
                continue;
            }
            // Pull(PN-04D6197A, SP-9525C4C0 §3) - 로컬에 할 일이 정말
            // 없을 때만(위 drainOnce가 false) 가장 바쁜 다른 코어의
            // gNormalQueues에서 하나 훔쳐온다. "~100ms 이상 idle 지속"
            // (PL-2D3184BC 원안)은 설계 단계에서 불필요한 복잡도로
            // 판단해 v1은 채택하지 않았다(매 idle 진입마다 즉시
            // 시도 - O(코어 수) 스캔 한 번뿐이라 비용이 낮다).
            const uint32_t victimCore = kFindMostLoadedCoreNumaAware(coreIndex);
            if (victimCore != coreIndex) {
                Task* stolen = gNormalQueues[victimCore].popMin();
                if (stolen) {
                    if (kCanMigrateFpuSafely(stolen, victimCore)) {
                        // pickNext()와 동일한 관례 - 큐에서 실제로
                        // 빠져나오는 순간 inRunQueue를 내린다.
                        stolen->inRunQueue = false;
                        next = stolen;
                    } else {
                        // FPU 소유권이 아직 살아있다(§5.3/§6-항목3) -
                        // 이번엔 보류하고 원래 큐로 되돌려 놓는다(vruntime
                        // 정렬 큐라 원래와 같은 자리로 돌아간다 - vruntime
                        // 자체를 안 건드렸으므로). popMin과 이 insert
                        // 사이 inRunQueue를 건드리지 않으므로(계속 true)
                        // 그 사이 다른 코어가 같은 Task를 이중 스케줄링할
                        // 수 없다.
                        gNormalQueues[victimCore].insert(stolen);
                    }
                }
            }
        }
        if (!next) {
            asm volatile("sti; hlt");
            continue;
        }
        // cli - gCurrentTask를 세팅한 시점과 실제로 next의 스택으로
        // 넘어가는 시점(kContextSwitch 내부의 mov rsp,rsi) 사이에 이
        // 코어의 틱이 끼어들면, onTick이 "next가 이미 실행 중"이라고
        // 착각해 아직 idle 스택 위에 있는 이 kContextSwitch 호출을
        // next 자신의 것처럼 다시 가로채어 버린다(next->tcb가
        // idle 스택의 스냅샷으로 덮어쓰임 - 실측으로 발견한 버그).
        // 여기서 끝 인터럽트는 kContextSwitch의 pushfq/popfq를 통해
        // idle 쪽에만 저장되고(나중에 idle이 재개될 때만 다시 반영),
        // next는 자신이 마지막으로 저장해 둔 RFLAGS(보통 IF=1)로
        // 독립적으로 재개되므로 next 쪽으로 "인터럽트 꺼짐"이 새어
        // 나가지 않는다.
        asm volatile("cli");
        {
            RwSpinlockWriteGuard guard(gCurrentTaskLock[coreIndex]);
            gCurrentTask[coreIndex] = next;
        }
        next->state = TaskState::Running;
        // [갱신, 2026-09-20, PN-81E49523 2단계] 예전엔 여기서 CR3를
        // 건드리지 않았다 - "이 kContextSwitch가 도착하는 지점(최초
        // 실행이면 kTaskStartTrampoline의 kSyncCr3OnTaskStart 호출,
        // yieldCurrent/parkCurrent 재개면 그 함수들 자신의 재개 지점)이
        // 전부 자기 자신의 안전한 스택으로 넘어온 뒤 스스로 CR3를
        // 동기화한다"는 신뢰(SP-83A07867 §3.2) 때문이었다. 그런데
        // `kContextSwitchFromISR` 도입으로 이 신뢰가 깨졌다 - onTick()/
        // onForcedMigration()에 의해 "current"로 캡처됐던 Task가 이
        // 자리에서 next로 뽑히면, 그 Task의 tcb는 실제 원래 인터럽트
        // 지점(순수 ring3/커널 코드, 스스로 CR3를 동기화하는 코드가
        // 전혀 없음)을 그대로 담고 있다 - 그래서 이제는 **모든 디스패처가
        // 예외 없이 `kSyncCr3(next)`를 불러야 한다**는 단순한 규칙으로
        // 통일했다(onTick()/onForcedMigration()의 해당 주석 참고 -
        // 그쪽은 이미 그렇게 하고 있었다). `kSyncCr3`의 skip-if-same
        // 최적화 덕분에 실제로 CR3가 그대로인 흔한 경우(kTaskStartTrampoline/
        // yieldCurrent/parkCurrent처럼 스스로도 동기화하는 도착 지점)엔
        // 추가 비용이 없다.
        kSyncRsp0ForDispatch(next);
        kSyncCr3(next);
        kContextSwitch(&gIdleTask[coreIndex].tcb, next->tcb);
        // yieldCurrent()로 되돌아온 경우에만 이 지점으로 온다(onTick의
        // Task-to-Task 직접 전환은 이 프레임을 거치지 않는다) - 다음
        // 루프에서 pickNext가 새 상태를 다시 판단한다. 이 시점의
        // 인터럽트 상태는 idle이 마지막으로 저장했던 그대로(위 cli로
        // 꺼져 있음)이므로, 아래에서 다시 준비 없이 바로 다음
        // pickNext/전환으로 넘어가도 안전하다 - sti는 "정말 대기할
        // 때"(위 hlt 분기)에만 한다.
        {
            RwSpinlockWriteGuard guard(gCurrentTaskLock[coreIndex]);
            // [갱신, 2026-09-19, PN-D47FBB8D] idle 자신으로 돌아왔다는
            // 사실을 nullptr이 아니라 실제 Task 포인터로 반영한다 -
            // gCurrentTask가 다시는 bare nullptr이 되지 않아야
            // onTick()의 freeze/디버그 정지 검사가 idle로의 전환
            // 자체도 정상적으로(current==idle이면 그 검사들이 항상
            // false를 내는 것으로) 거친다.
            gCurrentTask[coreIndex] = &gIdleTask[coreIndex];
        }
    }
}

// [SP-9F1DB1D8 §7] 설계자 답변으로 같은 코어 락-프리 절충을 철회 -
// 이 접근도 예외 없이 읽기 락을 탄다.
Task* Scheduler::currentTask() {
    const uint32_t coreIndex = currentCoreIndex();
    RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
    return gCurrentTask[coreIndex];
}

void Scheduler::resyncDebugRegsForCurrentTask() {
    if (Task* current = currentTask()) {
        kSyncDebugRegs(current);
    }
}

void Scheduler::captureCurrentFrame(InterruptFrame* frame) {
    // [신규, 2026-09-21, PN-584DB994, 설계자 지시] 위 scheduler.h의
    // 문서 주석 참고 - 호출부(kIsrHandler)가 이미 "중첩 아님"을
    // 확인했다는 전제 하에, 그냥 지금 이 인터럽트가 정말로 트랩한
    // Task의 tcb에 그 프레임을 그대로 복사해 둔다. `TaskTcb`(=
    // `InterruptFrame`의 별칭)라 대입 연산자 하나로 176바이트 전체가
    // 복사된다 - `kContextSwitchFromISR`의 "저장" 절반과 완전히
    // 동일한 값을 만들지만, 훨씬 이른 시점(스케줄링 결정보다 먼저)에
    // 무조건 실행돼 그 결정이 재진입/지연되어도 tcb 자체는 항상
    // 최신 상태를 유지한다.
    Task* current = currentTask();
    if (current && current->tcb) {
        *current->tcb = *frame;
    }
}

Task* Scheduler::taskOnCore(uint32_t coreIndex) {
    RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
    return gCurrentTask[coreIndex];
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
    Task* current;
    {
        RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
        current = gCurrentTask[coreIndex];
    }
    if (!current) {
        asm volatile("sti");
        return;  // idle 컨텍스트에서 잘못 호출된 경우 - 할 일 없음
    }
    {
        RwSpinlockWriteGuard guard(gCurrentTaskLock[coreIndex]);
        // [갱신, 2026-09-19, PN-D47FBB8D] nullptr 대신 idle Task로 -
        // gIdleTask 선언부/onTick() 문서 주석 참고.
        gCurrentTask[coreIndex] = &gIdleTask[coreIndex];
    }
    enqueue(coreIndex, current);
    // PN-57CF48DB - idle로 떠나기 전, 아직 current 자신의 안전한
    // 스택 위에 있을 때 CR3를 미리 gBootPml4Phys로 되돌린다(위
    // kSyncCr3ForIdleTransition 문서 주석 참고).
    kSyncCr3ForIdleTransition();
    kContextSwitch(&current->tcb, gIdleTask[coreIndex].tcb);
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
    kSyncDebugRegs(current);
    kSyncFsBase(current);
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
    Task* current;
    {
        RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
        current = gCurrentTask[coreIndex];
    }
    if (!current) {
        asm volatile("sti");
        return;  // idle 컨텍스트에서 잘못 호출된 경우 - 할 일 없음
    }
    {
        RwSpinlockWriteGuard guard(gCurrentTaskLock[coreIndex]);
        // [갱신, 2026-09-19, PN-D47FBB8D] nullptr 대신 idle Task로.
        gCurrentTask[coreIndex] = &gIdleTask[coreIndex];
    }
    current->state = TaskState::Blocked;
    // yieldCurrent()와의 유일한 차이 - 어느 큐에도 넣지 않는다. 다시
    // 실행되려면 누군가 scheduleImmediate()/enqueue()로 명시적으로
    // 큐에 넣어야 한다(그 시점엔 이 Task가 어느 큐에도 없다는 게
    // 보장되므로 이중 스케줄링 걱정이 없다).
    // PN-57CF48DB - yieldCurrent()와 같은 이유로 여기서도 idle로
    // 떠나기 전에 CR3를 미리 gBootPml4Phys로 되돌린다.
    kSyncCr3ForIdleTransition();
    kContextSwitch(&current->tcb, gIdleTask[coreIndex].tcb);
    // SP-83A07867 §3.2 갈래②의 나머지 한 곳 - yieldCurrent()의 재개
    // 지점과 완전히 동일한 이유로 여기서도 CR3를 동기화한다(위
    // yieldCurrent() 주석 참고 - 이 함수가 첫 실제 소비자가 되기
    // 전까지는 아직 발현되지 않았던 공백이었다).
    kSyncCr3(current);
    kSyncFpu(current, coreIndex);
    kSyncDebugRegs(current);
    kSyncFsBase(current);
    // 누군가 깨워 runLoop이 이 Task를 다시 고를 때까지 여기서 멈춰
    // 있다가, 다시 선택되면 이 지점부터 재개된다 - yieldCurrent()와
    // 같은 이유로(위 주석 참고) 여기서도 명시적으로 다시 켜야 한다 -
    // 저장된 RFLAGS에 기대면 cli 때문에 IF=0인 채로 복원되어, 이
    // Task가 다시 파킹되기 전까지 이 코어의 인터럽트가 전부 막힌다.
    asm volatile("sti");
}

// [신규, 2026-09-20, PN-EA968DF0, QU-47A83CDF 답변] parkCurrent()와
// 계약은 동일(Blocked 전환은 호출부 책임, 어느 큐에도 안 넣음, idle로
// 전환)하지만, 이 함수 자신은 "지금 여기"(자기 자신의 C 콜스택 안)를
// kContextSwitch로 캡처하지 않는다 - 이미 하드웨어+isr_common_stub이
// 만들어 둔 진짜 `frame`을 그대로 `kContextSwitchFromISR`로
// `caller->tcb`에 복사해 넣는다. 그 결과 이 호출은 **절대 반환하지
// 않는다** - parkCurrent()처럼 "나중에 이 함수 지점으로 재개돼 CR3/
// FPU/디버그레지스터를 다시 맞추고 sti"하는 꼬리 코드 자체가 없다
// (그럴 필요가 없다 - `caller`가 나중에 다시 뽑히면 `onTick()`/
// `runLoop()`의 기존 디스패치 코드가 이미 `kSyncCr3(next)` 등을
// 전부 하고 있고, 이 Task는 `frame`이 가리키던 원래 ring3 지점으로
// 곧장 `iretq`되기 때문 - kContextSwitchFromISR로 캡처된 다른 모든
// Task와 완전히 동일하게 취급된다). 첫 소비자는
// `kHandleUserBreakpointHit()`(debug_session.cpp) - #DB ISR이 예전
// 처럼 자기 자신의 C 콜스택을 코어 공유 IST4 위에 얼어붙은 채로
// 남겨 두지 않고, 이 함수 호출이 끝나는 즉시(=반환하지 않고 곧장
// idle로 넘어가는 순간) IST4를 완전히 비워 다른 스레드의 #DB가
// 안전하게 재사용할 수 있게 한다.
[[noreturn]] void Scheduler::parkFromISR(Task* caller, InterruptFrame* frame) {
    asm volatile("cli");
    const uint32_t coreIndex = currentCoreIndex();
    {
        RwSpinlockWriteGuard guard(gCurrentTaskLock[coreIndex]);
        gCurrentTask[coreIndex] = &gIdleTask[coreIndex];
    }
    // parkCurrent()와 동일한 이유(PN-57CF48DB) - idle로 떠나기 전에
    // CR3를 미리 되돌린다.
    kSyncCr3ForIdleTransition();
    kContextSwitchFromISR(&caller->tcb, gIdleTask[coreIndex].tcb, frame);
    // 도달 불가 - kContextSwitchFromISR의 "복원" 절반이 곧장
    // isr_common_epilogue로 jmp해 idle 자신의 재개 지점(runLoop())에
    // 착지한다. kTaskOnFallingToEnd/retireCurrentTask와 동일한 방어적
    // 무한 루프.
    for (;;) {
    }
}

void Scheduler::retireCurrentTask() {
    // yieldCurrent()/parkCurrent()와 같은 이유로 cli - gCurrentTask를
    // 지우기 전에 clean-up 큐에 먼저 넣으면, 그 사이 끼인 스케줄러 틱이
    // 이 Task를 "아직 실행 중"으로 오인해 존재하지 않는 전환을
    // 시도할 위험이 있다.
    asm volatile("cli");
    const uint32_t coreIndex = currentCoreIndex();
    Task* current;
    {
        RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
        current = gCurrentTask[coreIndex];
    }
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
    {
        RwSpinlockWriteGuard guard(gCurrentTaskLock[coreIndex]);
        // [갱신, 2026-09-19, PN-D47FBB8D] nullptr 대신 idle Task로.
        gCurrentTask[coreIndex] = &gIdleTask[coreIndex];
    }
    current->state = TaskState::Zombie;
    // parkCurrent()와 달리 "누군가 깨워주길" 기다리는 게 아니라 다시는
    // 선택되지 않는다 - 이 Task의 커널 스택은 지금 이 kContextSwitch
    // 호출이 실제로 다른 스택으로 넘어가야(=더 이상 이 스택 위에서
    // 실행되지 않게 되어야) 비로소 안전하게 회수할 수 있으므로, 회수
    // 자체는 runLoop()이 idle 컨텍스트(다른 스택) 위에서 이 큐를
    // 드레인하며 나중에 처리한다.
    gCleanupQueues[coreIndex].pushBack(current);
    // PN-57CF48DB - yieldCurrent()/parkCurrent()와 같은 이유로 여기서도
    // idle로 떠나기 전에 CR3를 미리 gBootPml4Phys로 되돌린다 - 이
    // 지점은 재개가 없어(zombie, 다시 뽑히지 않음) 도착 지점에서
    // 뒤늦게 동기화할 기회 자체가 없다.
    kSyncCr3ForIdleTransition();
    kContextSwitch(&current->tcb, gIdleTask[coreIndex].tcb);
    // 이 지점으로 다시는 돌아오지 않는다(current는 이미 Zombie로
    // 어느 스케줄 큐에도 없어 다시 뽑힐 수 없다) - kAsyncTaskEntryWrapper
    // 와 동일한 패턴의 방어적 무한 루프.
    for (;;) {
    }
}

// [신규, 2026-09-18, PN-B5C2845A] SelfTerminateHandler(PN-40E976F2)
// 전용이던 "사망 전파" 로직을 공용 함수로 뽑았다 - scheduler.h의
// `Scheduler::cancelPendingSyscalls` 문서 주석 참고(두 호출부의 차이 -
// 자기 자신이 죽는 경우 vs 다른 프로세스의 Kill이 대상으로 삼는,
// 여전히 살아서 `Syscall::wait()`로 파킹된 경우 - 이 함수가 항목별로
// `waitingTask` 유무를 직접 확인해 두 경우 모두 안전하게 처리한다).
void Scheduler::cancelPendingSyscalls(UserThread* userThread) {
    userThread->pendingSyscalls.forEach([userThread](UserThread::PendingSyscall& pending, auto* slot) {
        auto* asyncTask = reinterpret_cast<AsyncTask*>(pending.token);
        // [신규, PN-B5C2845A] 이 AsyncTask가 끝나기를 실제로 기다리는
        // kernel::Task가 있는지(예: Kill 대상이 이 토큰으로
        // waitForAnyOf에 파킹돼 있는 경우) - 있으면 그 대기자가 나중에
        // 스스로 소비/반납하도록 이 항목을 건드리지 않고 목록에도
        // 남겨 둔다(대기자의 waitForAnyOf가 자기 pendingSyscalls에서
        // erase한다). SelfTerminateHandler의 원래 시나리오(자기 자신이
        // 죽는 경우)는 이 목록의 어떤 항목도 대기자를 가질 수 없다
        // (그 스레드 자신이 지금 실행 중이라 동시에 파킹돼 있을 수
        // 없으므로) - 그래서 이 확인을 추가해도 기존 동작은 전혀
        // 안 바뀐다.
        const bool hasWaiter = static_cast<bool>(asyncTask->waitingTask.lock());
        if (asyncTask->state == AsyncTaskState::Completed || asyncTask->state == AsyncTaskState::Failed) {
            if (hasWaiter) {
                return;  // 대기자가 스스로 소비/반납한다 - 여기서 먼저 반납하면 UAF.
            }
            // 이미 끝났지만 아무도 wait()로 가져가지 않은 결과 -
            // 리액터는 autoFree=false라 이미 손을 뗀 상태이므로
            // 여기서 대신 반납한다.
            GenericSlabAllocator::free(reinterpret_cast<void*>(asyncTask->stackBase), kAsyncTaskStackSize);
            if (asyncTask->tcb) {
                GenericSlabAllocator::free(asyncTask->tcb, sizeof(TaskTcb));  // PN-81E49523 2단계 - stackBase와 별도 할당
            }
            GenericSlabAllocator::free(asyncTask, sizeof(AsyncTask));
            userThread->pendingSyscalls.erase(slot);
            return;
        }
        // 아직 안 끝났다 - 취소로 전이한다. 대기자가 없으면(기존
        // 시나리오) autoFree를 강제로 켜서 리액터가 스스로 반납하게
        // 하고 목록에서도 지운다 - 있으면(신규 시나리오) 그대로
        // false로 남겨 대기자의 waitForAnyOf가 반납/erase하게 한다.
        const bool wasSuspended = (asyncTask->state == AsyncTaskState::Suspended);
        asyncTask->autoFree = !hasWaiter;
        // §8.3-1(SP-F682B889) - onCancel을 부르기 전에 먼저
        // cancelSource를 트리거한다. 이 AsyncTask가 Ready였다면
        // 아래에서 onExec 자체를 건너뛰므로 사실상 무관하지만,
        // Suspended였다면(yield()로 실행 중간에 멈춰 있었다면)
        // onCancel만 불리고 onExec으로는 다시 재개되지 않으므로
        // (state==Cancelled 검사가 onExec 재개보다 우선) 이 트리거
        // 자체가 onExec 쪽에서 관측될 일은 없다 - 그래도 §8.2가
        // "이미 끝난 작업에 트리거해도 무해"를 보장하고, cancelSource
        // 를 직접 폴링하는 다른 코드(예: 타임아웃과 경합하는 코드)
        // 가 상태를 일관되게 보게 하기 위해 항상 호출한다.
        asyncTask->cancelSource.trigger();
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
        if (!hasWaiter) {
            userThread->pendingSyscalls.erase(slot);
        }
    });
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
    // [신규, 2026-09-20, PN-81E49523 2단계] tcb가 이제 커널 스택과
    // 완전히 별도의 Slab 할당 - 위 커널 스택 반납과 별개로 반드시
    // 여기서도 반납해야 새지 않는다.
    if (task->tcb) {
        GenericSlabAllocator::free(task->tcb, sizeof(TaskTcb));
        task->tcb = nullptr;
    }
}

void Scheduler::disablePreemption() {
    ++gPreemptDisableCount[currentCoreIndex()];
}

void Scheduler::enablePreemption() {
    uint32_t& count = gPreemptDisableCount[currentCoreIndex()];
    if (count > 0) {
        --count;
        // [신규, 2026-09-17, PN-495C11B7, SP-B1E258D8 §5.1] 카운터가
        // 방금 0으로 돌아온 바로 이 순간이 RCU의 quiescent state다 -
        // 호출부(RcuReadGuard/PreemptionGuard 사용자)가 RCU를 몰라도
        // 되도록 여기서 대신 기록한다.
        if (count == 0) {
            Rcu::noteQuiescentStateOnThisCore();
        }
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
    Task* current;
    {
        RwSpinlockReadGuard guard(gCurrentTaskLock[coreIndex]);
        current = gCurrentTask[coreIndex];
    }
    if (!current) {
        return;  // idle 컨텍스트는 FPU/SSE를 사용하지 않는다 - 이론상 도달 불가
    }
    Task* owner = gFpuOwner[coreIndex];
    if (owner == current) {
        return;  // 이미 이 Task가 소유자인데 걸린 가짜 트랩(kSyncFpu가 놓친 경우 없음) - 방어적 처리
    }
    if (owner) {
        // owner가 gFpuOwner인 이상 fpuContext는 이미 non-null이다
        // (kEvictFpuBeforeMigration과 동일한 불변조건).
        asm volatile("fxsave (%0)" : : "r"(owner->fpuContext->buffer) : "memory");
    }
    if (current->fpuContext) {
        asm volatile("fxrstor (%0)" : : "r"(current->fpuContext->buffer) : "memory");
    } else {
        // 이 Task가 FPU/SSE를 사용하는 게 처음이다 - 이전 소유자가 남긴
        // 낡은 상태를 물려받지 않도록 깨끗한 초기 상태로 시작한다.
        asm volatile("fninit");
        // [신규, 2026-09-19, PN-8726CDBD] 지연 할당 - 이 Task가 FPU를
        // 처음 쓰는 이 순간에만 TaskFpuContext를 슬랩에서 확보한다.
        // 할당 실패(극히 드문 슬랩 고갈)는 커널을 패닉시키지 않고
        // 그냥 이번엔 fpuContext를 비워 둔 채로 넘어간다 - current는
        // 방금 fninit으로 이미 깨끗한 하드웨어 상태이므로 즉시 잘못된
        // 동작을 하지는 않고, 다음 #NM 트랩에서 할당을 다시 시도한다
        // (메모리 압박이 풀리면 자연히 회복 - 이 실패 경로에 별도
        // 에러 보고 채널이 없어 조용히 재시도하는 것이 유일한 선택).
        auto* raw = static_cast<TaskFpuContext*>(GenericSlabAllocator::alloc(sizeof(TaskFpuContext)));
        if (raw) {
            current->fpuContext = kMakeUnique(raw);
        }
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
        kernel::kSyncDebugRegs(self);
        kernel::kSyncFsBase(self);
    }
}

// context_switch.S의 kTaskFallingToEnd(entry가 반환해 Task 실행이
// 자연 종료되는 지점)가 호출한다 - PL-2D3184BC "Task 종료 프로토콜"
// (QU-26F9420E 설계자 답변, 2026-09-14)의 두 분기를 그대로 구현한다.
// 이 함수 자체가 반환하면(User-Level 분기) 호출부가 이어서 hlt
// 루프로 들어간다 - Kernel-Level 분기(retireCurrentTask())는 절대
// 반환하지 않는다.
extern "C" void kTaskOnFallingToEnd() {
    // [신규, 2026-09-19, PN-05162577] entry()가 막 반환한 이 시점부터
    // 아래에서 이 Task가 실제로 Zombie로 표시되거나(Kernel-Level) 자기
    // 종료 syscall 제출을 마치기(User-Level) 전까지, gCurrentTask/
    // 스케줄 큐 관점에서 이 Task는 여전히 "정상 실행 중"으로 보인다 -
    // 이 좁은 창에 스케줄러 틱이 끼어들면 onTick()이 이 Task를 보통의
    // 선점 대상으로 오인해 그대로 재삽입(enqueue)하고 다른 Task로
    // 전환해 버릴 수 있다(퇴역 결정 자체를 아직 아무도 안 내렸으므로
    // Zombie 분기도 안 타 정상 경로로 처리됨). 나중에 이 Task가 다시
    // 뽑히면 이 지점(정확히는 onTick()의 kContextSwitch 호출부) 한
    // 가운데서 재개되어 커널 스택이 두 실행 흐름에 동시에 걸치는
    // 위험한 상태가 된다 - retireCurrentTask() 자신의 cli 문서 주석이
    // 설명하는 것과 완전히 같은 위험을, 그 cli보다 한 단계 앞선
    // 지점(entry 반환 직후)부터 이미 열어 두고 있었던 셈. 그 cli를
    // 여기 앞당겨 창을 완전히 닫는다 - User-Level 분기는 아래 hlt
    // 루프가 인터럽트로 깨어나야 하므로 반환 직전 sti로 다시 연다.
    asm volatile("cli");
    kernel::Task* self = kernel::Scheduler::currentTask();
    if (!self) {
        asm volatile("sti");
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
        asm volatile("sti");  // 위 cli를 닫는다 - 아래 hlt 루프는 인터럽트로만 깨어난다.
        return;
    }
    // Kernel-Level Task가 계속 커널에 머물러 있는 경우(설계자 지시
    // 2번, 지금 이 프로젝트의 모든 Task가 해당) - 절대 돌아오지 않는다.
    // retireCurrentTask() 자신도 cli를 실행하지만 위에서 이미 꺼 둔
    // 상태라 그저 무해한 재확인이다.
    kernel::Scheduler::retireCurrentTask();
}

// [신규, 2026-09-18, SP-76250478 §3 항목2, PN-0EB2FABF] `kTaskOnFallingToEnd`
// (위)의 스레드 전용 대칭 - `idt.cpp`의 `kDispatchSyscallVerbBody`가
// `kSyscallEndpointSelfTerminateThread` submit을 가로챌 때만 부른다
// (자연 종료/`syscall` fast path 어느 쪽에서도 호출되지 않는다 - 이
// syscall은 언제나 명시적이라 자연 반환 경로 자체가 없다). `exitCode`
// 를 받는다는 점만 `kTaskOnFallingToEnd`와 다르다 - `SelfTerminate`는
// 아직 종료 코드를 안 쓰지만(그 syscall 문서 주석 참고) `SelfTerminateThread`
// 는 §3.1의 `Join`이 돌려줄 값이 필요해 애초부터 받는다. Kernel-Level
// Task 분기가 없다 - 이 syscall은 항상 UserThread 실행 흐름에서만 온다
// (`syscall.h`의 `Syscall` 클래스 문서 주석과 동일한 전제).
extern "C" void kThreadOnFallingToEnd(kernel::int32_t exitCode) {
    kernel::Task* self = kernel::Scheduler::currentTask();
    if (!self || !self->isUserLevel) {
        return;  // 이론상 도달 불가 - 방어적으로 그냥 hlt 루프로
    }
    auto* userThread = static_cast<kernel::UserThread*>(self);
    // [PN-0EB2FABF] `SelfTerminateThreadHandler`가 나중에 리액터
    // 컨텍스트에서 읽을 exitCode를 미리 심어 둔다 - 별도 힙 할당 없이
    // `Task*` 하나만 args로 넘기는 기존 `kTaskOnFallingToEnd` 관례를
    // 그대로 재사용하기 위함(UserThread::exitCode 필드는 이미 Phase 1
    // 에서 마련돼 있었다).
    userThread->exitCode = exitCode;
    self->state = kernel::TaskState::Zombie;
    kernel::Syscall::submitDetached(kernel::kSyscallEndpointSelfTerminateThread, self);
}
