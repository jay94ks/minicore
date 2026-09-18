#include "async_task.h"

#include "acpi.h"
#include "delayed_exec.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "lapic.h"
#include "libkenv/mem.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"
#include "paging.h"
#include "rcu.h"
#include "scheduler.h"
#include "syscall.h"
#include "task.h"

namespace {

extern "C" void kTaskStartTrampoline();

// [신규, PN-523B779F 조사 중 발견/수정] onExec()이 제출자(UserThread)의
// 유저 포인터를 직접 역참조하는 게 이 코드베이스 전반의 기존 관례인데
// (channel.cpp의 kValidateUserBuffer/kResolveOwnedBridge, pnp.cpp의
// kValidateEnumerateBuffer 등 - 전부 "포인터가 그 주소공간에 속하는지"
// 검증만 하고, 실제 그 CR3로 전환하는 건 이 진입 래퍼의 몫이라고
// 암묵적으로 가정해 왔다), 정작 그 CR3 동기화 자체가 어디에도 없었다 -
// `Scheduler::runLoop()`이 idle로 들어가기 전에 CR3를 `gBootPml4Phys`
// 로 되돌려 두므로(scheduler.cpp 1218행 부근), 그 뒤 리액터가
// `drainOnce()`로 이 AsyncTask를 처음 실행할 때 CR3가 제출자의 유저
// 주소공간이 아니라 `gBootPml4Phys`인 채로 `onExec()`이 실행돼, 제출자의
// 유저 스택(예: syscall args를 담은 지역 변수)을 가리키는 포인터를
// 그대로 역참조하면 그 주소가 `gBootPml4Phys`엔 아예 매핑돼 있지 않아
// Page Fault -> (ring0에서 난 폴트라 `kTerminateFaultingUserTask`
// 경로를 못 타고) `kPanic`까지 간다 - devmgr의 EnumerateDevices
// 첫 실사용으로 실측 발견(PN-BD9AAE2F 3번 항목).
//
// **여기(스택풀 최초 진입, 아래 kAsyncTaskEntryWrapper) 한정으로만
// 고친다** - 이 함수는 `kContextSwitch`로 AsyncTask 자신의 전용
// 스택(GenericSlabAllocator 커널 힙 메모리, 모든 프로세스의 PML4에
// 공유되는 higher-half 안에 있어 CR3가 뭐든 항상 안전하게 접근
// 가능)으로 이미 넘어온 뒤라 CR3를 바꿔도 다음 스택 접근이 위험하지
// 않다(`kSyncCr3`가 "onTick()에서만 안전, runLoop()에서는 위험"이라고
// 경고하는 그 위험한 부트 스택과는 다른 스택 - scheduler.cpp의
// `kSyncCr3` 문서 주석 참고). **`AsyncReactor::drainOnce()`의
// `coroHandle.resume()` 재개 경로(co_await로 suspend됐다 나중에
// 재개되는 경우, 예: ChannelReadHandler의 대기 루프)는 일부러 손대지
// 않았다** - 그 경로는 리액터 자신이 지금 서 있는 스택 위에서 직접
// resume()하므로(스택 전환 없음, `async_task.h`의 "코루틴 방식이
// 스택풀 방식보다 가벼운 핵심 이유" 주석 참고), `drainOnce()`가
// `Scheduler::runLoop()`의 idle 인라인 호출에서 불렸다면 그 스택이 바로
// 위 "위험한 부트 스택"일 수 있어 여기와 같은 방식으로 안전하게 CR3를
// 바꿀 수 없었다 - 별도 설계가 필요한 남은 범위로 PN-523B779F에 기록해
// 뒀다.
//
// **[정정, 2026-09-17, PN-2008220B]** 그 "위험한 부트 스택" 자체가
// 이제는 없다 - `Scheduler::enterIdleLoop()`이 이 코어의 idle
// 컨텍스트를 higher-half 전용 스택(`gIdleStack`, 모든 PML4에 공유)
// 으로 영구히 옮겨 놓았으므로, `coroHandle.resume()`이 그 스택 위에서
// CR3를 바꿔도 더 이상 "다음 스택 접근이 즉시 폴트"라는 위험은 없다.
// **다만 이 함수(CR3 동기화 로직)를 그 재개 경로에도 적용하는 작업
// 자체는 아직 하지 않았다** - 스택 안전성이라는 유일한 장애물은
// 사라졌지만 "resume() 전에 어느 Task 기준으로 동기화할지 / resume()
// 이 끝나고 drainOnce()로 돌아온 뒤 CR3를 어떻게 원복할지"는 이 함수를
// 그대로 재사용할 수 없는 별도의 설계가 필요해 PN-523B779F "남은 범위"
// 에 그대로 열어 두고 그 계획에도 이 사실을 교차 기록해 뒀다 - 이번
// 수정의 스코프는 "스택을 안전하게 만드는 것"까지다.
kernel::uint64_t kSyncCr3ForAsyncExecEntry(kernel::AsyncTask* task) {
    const kernel::uint64_t original = kernel::Paging::currentPml4Phys();
    kernel::SharedPtr<kernel::Task> submitter = task->submitterTask.lock();
    if (submitter) {
        auto* thread = static_cast<kernel::UserThread*>(submitter.get());
        if (thread->isUserLevel && thread->userPml4Phys && thread->userPml4Phys != original) {
            asm volatile("mov %0, %%cr3" : : "r"(thread->userPml4Phys) : "memory");
        }
    }
    return original;
}

void kRestoreCr3AfterAsyncExecEntry(kernel::uint64_t original) {
    if (kernel::Paging::currentPml4Phys() != original) {
        asm volatile("mov %0, %%cr3" : : "r"(original) : "memory");
    }
}

// AsyncTask 전용 진입 래퍼 - kTaskStartTrampoline(context_switch.S)이
// "call rbx"로 이 함수를 호출한다(rdi = r12 = AsyncTask* 그대로). Task와
// 달리 이 함수가 "반환"하면 안 된다(반환하면 트램폴린의 halt 루프로
// 떨어져 이 코어가 영원히 멈춘다) - 그래서 onExec 실행 뒤 반드시
// AsyncTask::yield()류의 kContextSwitch로 리액터에 영구 복귀한다.
void kAsyncTaskEntryWrapper(void* arg) {
    auto* task = static_cast<kernel::AsyncTask*>(arg);
    // [PN-523B779F] onExec() 호출 전 제출자의 유저 주소공간으로 CR3를
    // 맞춘다 - 위 kSyncCr3ForAsyncExecEntry 문서 주석 참고. handler가
    // 없어도(아래 else 분기) 호출 자체는 무해하므로 조건 없이 부른다.
    const kernel::uint64_t savedPml4ForAsyncExec = kSyncCr3ForAsyncExecEntry(task);
    kernel::AsyncTaskHandler* handler = kernel::AsyncCallbackRegistry::resolve(task->subjectCode);
    if (handler) {
        // [PN-C62F7908, 2/5 -> 4/5 -> 5/5] onExec이 AsyncExecCoro를
        // 반환한다(§7.3) - final_suspend=SuspendAlways라 자동으로
        // 정리되지 않으므로 done()을 직접 확인해야 한다.
        kernel::AsyncExecCoro result = handler->onExec(task, task->args);
        // [PN-C62F7908 5/5, §7.2 확정] result.handle()이 비어 있으면
        // 코루틴 프레임 Slab 할당이 실패한 것(promise_type::
        // get_return_object_on_allocation_failure() 경로, done()이
        // 아니라 handle()로 구분해야 한다 - done()은 빈 핸들도 true로
        // 본다). onExec() 본문은 이 경우 전혀 실행되지 않았으므로(실측
        // 확인, §7.2) 부작용 걱정 없이 그냥 다시 호출하면 된다 - 이
        // AsyncTask 자신이 yield()로 리액터에 제어를 돌려준 뒤 나중에
        // 다시 스케줄링(kContextSwitch로 재개)되면 바로 이 지점에서
        // 재시도가 이어진다.
        while (!result.handle()) {
            // **실측으로 찾은 버그(AsyncTaskAwaiter::await()가 이미 문서화한
            // 것과 정확히 같은 종류)**: `AsyncTask::yield()`만 부르면 그
            // 뒤 아무도 이 AsyncTask를 다시 큐에 넣어 주지 않아 영원히
            // Suspended로 멈춘다 - AsyncTaskAwaiter::await()와 동일한
            // 패턴으로, yield() 직전 스스로를 다시 제출해(drainOnce()가
            // 이미 지원하는 "Suspended면 Running으로 되돌려 재개" 경로
            // 재사용) 다음 드레인 차례에 반드시 다시 뽑히게 한다.
            kernel::AsyncReactor::submitCompletion(task);
            kernel::AsyncTask::yield();
            // [PN-523B779F] yield() 동안 다른 Task/AsyncTask가 CR3를
            // 바꿔 놨을 수 있어 재시도 직전에 다시 맞춘다.
            kSyncCr3ForAsyncExecEntry(task);
            result = handler->onExec(task, task->args);
        }
        if (result.done()) {
            // co_await를 안 썼거나(전부 co_return만 씀) 이미 완료 -
            // 기존 그대로 즉시 정리.
            result.destroy();
            task->state = kernel::AsyncTaskState::Completed;
        } else {
            // [PN-C62F7908 4/5] 첫 co_await에서 suspend됐다 - 이
            // AsyncTask의 전용 스택(이 함수 자신이 실행 중인 스택)은
            // 여기서 마지막으로 쓰인다: 아래 yield()로 리액터에 복귀한
            // 뒤 이 지점으로는 다시는 안 돌아온다(더 이상 kContextSwitch
            // 로 이 스택을 재개하지 않기 때문) - 이후 재개는 전부
            // AsyncReactor::drainOnce()의 `coroHandle.resume()` 경로
            // (리액터 자신의 스택 위에서 직접 실행, 스택 전환 없음)로만
            // 일어난다. coroHandle을 여기서 저장해 둬야 drainOnce()가
            // 그 경로를 고를 수 있다.
            task->coroHandle = result.handle();
            task->state = kernel::AsyncTaskState::Suspended;
        }
    } else {
        // 등록되지 않은 subjectCode로 제출된 경우 - 설계/구현 오류지만
        // 이 코어를 멈추지 않기 위해 실패로만 표시하고 계속 진행한다.
        task->state = kernel::AsyncTaskState::Failed;
    }
    // [PN-523B779F] 리액터/idle 컨텍스트로 돌아가기 전 CR3를 이 함수
    // 진입 시점 값으로 되돌린다 - runLoop()의 idle 분기가 요구하는
    // "idle 컨텍스트에서는 항상 gBootPml4Phys(또는 이 함수를 부른
    // 시점의 원래 값)"라는 기존 불변조건을 그대로 지킨다.
    kRestoreCr3AfterAsyncExecEntry(savedPml4ForAsyncExec);
    kernel::AsyncTask::yield();
    // yield()가 이 AsyncTask를 다시 스케줄하지 않으므로(리액터가 상태를
    // 보고 정리) 이 지점으로 다시는 돌아오지 않는다 - 방어적 무한 루프.
    for (;;) {
    }
}

constexpr kernel::uint32_t kMaxCores = kernel::kAcpiMaxCpus;

// AsyncTask::next 침습적 포인터를 재사용하는 단일 연결 리스트 -
// scheduler.h의 TaskQueue와 정확히 같은 패턴(Spinlock 폴백).
class AsyncTaskQueue {
public:
    void pushBack(kernel::AsyncTask* task) {
        kernel::SpinlockGuard guard(_lock);
        task->next.store(nullptr);
        if (_tail) {
            _tail->next.store(task);
        } else {
            _head = task;
        }
        _tail = task;
    }

    kernel::AsyncTask* popFront() {
        kernel::SpinlockGuard guard(_lock);
        kernel::AsyncTask* task = _head;
        if (task) {
            _head = task->next.load();
            if (!_head) {
                _tail = nullptr;
            }
            task->next.store(nullptr);
        }
        return task;
    }

private:
    kernel::Spinlock _lock;
    kernel::AsyncTask* _head = nullptr;
    kernel::AsyncTask* _tail = nullptr;
};

AsyncTaskQueue gExecQueues[kMaxCores];

// 선점 큐(SP-00CA7175 §2.2, PN-7AC01E6E 항목 6) - exclusivePreemptive
// Channel의 connectChannel/acceptFromChannel 핸드셰이크 완료에서만
// 채워진다(async_task.h의 submitCompletion 주석 참고). reactorTaskEntry가
// 매 루프마다 이 큐를 gExecQueues보다 먼저 비운다 - 그 외에는 완전히
// 동일한 큐 구현을 재사용.
AsyncTaskQueue gPreemptiveQueues[kMaxCores];

// 이 코어에서 지금 실행/재개 중인 AsyncTask - reactorTaskEntry만
// 갱신한다. nullptr이면 리액터가 popFront()/parkCurrent() 사이 어딘가.
kernel::AsyncTask* gCurrentAsyncTask[kMaxCores] = {};

// 이 코어에서 "지금 이 자리"(runLoop()의 idle 인라인 호출이든, §4 (C)
// 경로의 IPI 핸들러든)가 AsyncTask로 전환하기 직전의 자기 자신 RSP -
// AsyncTask::yield()가 돌아올 자리(옛 "리액터 Task 컨텍스트"와 정확히
// 같은 역할, 이름만 유지 - 별도 Task가 아니게 됐다고 해서 이 슬롯
// 자체의 의미가 바뀌지는 않는다).
kernel::uint64_t gReactorSavedRsp[kMaxCores] = {};

// AsyncReactor::drainOnce()의 재진입 방지 플래그(2026-09-16 재구조,
// PN-FEAAF154) - 이미 이 코어에서 드레인이 진행 중일 때(gReactorSavedRsp
// 슬롯이 사용 중일 때) §4 (C) 경로의 IPI가 겹쳐 들어와도 또 다른
// drainOnce() 호출이 같은 슬롯을 건드리지 않도록 막는다. 옛
// `gReactorParked`(리액터 Task의 park/wake 상태 추적)와는 목적이
// 다르다 - Task 자체가 없어졌으므로 그 개념은 폐기됐다.
bool gDraining[kMaxCores] = {};

// §4 (C) 경로("다른 Task 실행 중일 때"의 즉시 개입, QU-3BDEE348 답변)
// 전용 IPI 벡터 - RM-28225668에 배정(0xE3, kForcedMigrationVector
// (SP-ECC59BAE, 0xE2) 다음 번호). ISR은 이 코어 자신에게만 보내는
// self-IPI를 받는다 - x86 표준 동작대로 인터럽트가 다시 켜지는 순간
// (대개 submitCompletion() 호출부가 반환하며 sti하는 시점, 또는 이미
// sti 상태였다면 그 즉시) 전달된다.
constexpr kernel::uint32_t kAsyncDrainVector = 0xE3;

void kAsyncDrainIsr(kernel::InterruptFrame*) {
    const kernel::uint32_t coreIndex = kernel::Scheduler::currentCoreIndex();
    // 이 IPI가 도착한 시점에 큐에 있던 것 전부를 이 자리에서 처리한다 -
    // 그 사이 또 들어온 게 있으면(드문 경쟁) 다음 IPI가 마저 처리하므로
    // 무한정 여기 머무르지 않는다. EOI는 공통 ISR 스텁이 처리
    // (tlb_shootdown.cpp와 동일 관례).
    while (kernel::AsyncReactor::drainOnce(coreIndex)) {
    }
}

constexpr kernel::uint32_t kMaxHandlers = 64;  // v1 상한 - 필요해지면 늘림
kernel::AsyncTaskHandler* gHandlers[kMaxHandlers] = {};
kernel::uint32_t gNextSubjectCode = 0;
kernel::Spinlock gRegistryLock;

}  // namespace

namespace kernel {

// [PN-D01B7D07, SP-F682B889 §3.7] 헤더의 전방 선언(async_task.h)에
// 대응하는 실제 정의 - AsyncTask 자신의 수명과 완전히 독립적으로
// 힙에 할당되는 작은 컨트롤 블록. 정확히 두 참여자가 있다: (1)
// AsyncTask 자신(생성한 쪽, `task->weakRef`로 들고 있음), (2)
// `DelayedExecutionQueue`에 예약된 타임아웃 콜백(`kOnAsyncTaskTimeout`).
// 락 없이 두 원자 연산만으로 "AsyncTask가 아직 살아있는지"와 "이
// 컨트롤 블록 자체를 누가 마지막으로 다 썼는지"를 둘 다 안전하게
// 판정한다:
//
// - `lock()` - 타이머 콜백이 발화 시점에 부른다. 아직 무효화되지
//   않았으면 그 순간의 `AsyncTask*`를 반환(그 뒤로도 안전하게
//   역참조할 수 있다 - 왜 안전한지는 `invalidate()`가 반드시
//   `kReleaseAsyncTask()`"안에서" 실제 반납보다 **먼저** 불린다는
//   보장 덕분이다: `lock()`이 non-null을 반환했다는 건 그 반환
//   시점에 아직 무효화 전이었다는 뜻이고, 같은 코어 위에서 순차
//   실행되는 이 커널에 진짜 동시 실행 경쟁은 없다 - 타임아웃
//   콜백도 `DelayedExecutionQueue::pump()`도 전부 리액터의 idle
//   경로에서만 실행되는 협조적 스케줄링이라 인터럽트 컨텍스트를
//   제외하면 서로 겹치지 않는다).
// - `invalidate()` - AsyncTask가 실제로 반납되는 바로 그 순간
//   (`kReleaseAsyncTask()`)에만 부른다 - 이후 `lock()`은 항상
//   nullptr.
// - `release()` - "이 컨트롤 블록 자체"의 참조 카운트를 하나 줄이고,
//   0이 되면(마지막 참여자) 블록 자신을 반납한다. 생성 시 2로
//   시작(AsyncTask 쪽 몫 1 + 타이머 콜백 쪽 몫 1) - 둘 다 각자 볼일이
//   끝나면(AsyncTask는 반납 시, 타이머는 발화 시) 정확히 한 번씩
//   `release()`를 불러야 한다. `DelayedExecutionQueue::cancel()`이
//   전혀 필요 없다는 게 이 설계의 핵심 - 타이머는 항상 예정대로
//   발화하고, 이미 끝난 AsyncTask를 가리키면(`lock()==nullptr`)
//   그냥 조용히 자기 몫만 `release()`하고 끝난다.
class AsyncTaskWeakRef {
public:
    // 이 프로젝트 전역 관례대로 placement new를 쓰지 않는다(raw slab
    // 메모리 위에 reinterpret_cast로 앉힌 뒤 명시적으로 초기화 -
    // AsyncTask::init()/chunked_list.h와 동일한 패턴).
    void init(AsyncTask* target) {
        _target.store(target);
        _refCount.store(2);  // AsyncTask 쪽 몫 1 + 타이머 콜백 쪽 몫 1
    }

    AsyncTask* lock() const { return _target.load(); }
    void invalidate() { _target.store(nullptr); }

    void release() {
        if (_refCount.fetchSub(1) == 1) {
            GenericSlabAllocator::free(this, sizeof(AsyncTaskWeakRef));
        }
    }

private:
    AtomicPtr<AsyncTask> _target;
    AtomicU32 _refCount;
};

namespace {

void kOnAsyncTaskTimeout(void* arg) {
    auto* weakRef = static_cast<AsyncTaskWeakRef*>(arg);
    AsyncTask* task = weakRef->lock();
    if (task) {
        // §8.3 세 번째 트리거 지점(QU-681F256C 답변 - "Cancel Source
        // 쪽에 timeout을 유발") - Failed/Cancelled 상태 전이 자체는
        // 기존 협조적 취소 채널(cancelSource를 각 handler의 onExec/
        // 코루틴 본문이 스스로 확인)이 그대로 처리한다, 이 콜백은
        // 트리거만 담당.
        task->cancelSource.trigger();
    }
    weakRef->release();
}

}  // namespace

void AsyncTask::scheduleTimeout(uint64_t delayTicks) {
    if (weakRef) {
        return;  // v1 - 이미 걸려 있으면 두 번째 호출은 무시(설계 문서 그대로)
    }
    void* mem = GenericSlabAllocator::alloc(sizeof(AsyncTaskWeakRef));
    if (!mem) {
        return;  // 할당 실패 - 타임아웃 없이 계속 진행(치명적이지 않음)
    }
    weakRef = reinterpret_cast<AsyncTaskWeakRef*>(mem);
    weakRef->init(this);
    DelayedExecutionQueue::schedule(delayTicks, &kOnAsyncTaskTimeout, weakRef);
}

// [PN-D01B7D07] 이 AsyncTask를 실제로 반납하는 유일한 통로 - 기존에
// 4곳(AsyncTaskGroup::pendingCount()/AsyncTaskAwaiter::~AsyncTaskAwaiter/
// drainOnce()의 Cancelled/Completed·Failed 분기)에 각자 따로 있던
// "스택 반납 + 구조체 반납" 두 줄을 여기로 모았다 - weakRef 무효화를
// **정확히 한 곳**에서만 하면 되게 하기 위함(그렇지 않으면 4곳
// 전부에 빠짐없이 배선해야 해 QU-0CB8CAEE가 지적한 "새 필드 최소화"
// 취지와 다시 부딪힌다).
void kReleaseAsyncTask(AsyncTask* task) {
    if (task->weakRef) {
        task->weakRef->invalidate();
        task->weakRef->release();
        task->weakRef = nullptr;
    }
    GenericSlabAllocator::free(reinterpret_cast<void*>(task->stackBase), kAsyncTaskStackSize);
    GenericSlabAllocator::free(task, sizeof(AsyncTask));
}

void AsyncTask::init(AsyncTaskSubjectCode subjectCodeIn, AsyncTaskManageCode manageCodeIn, void* argsIn) {
    subjectCode = subjectCodeIn;
    manageCode = manageCodeIn;
    args = argsIn;
    state = AsyncTaskState::Ready;
    next.store(nullptr);
    // AsyncTask는 항상 raw slab 메모리 위에 reinterpret_cast로 앉혀지고
    // (placement new 없음) - 기본 멤버 초기화식은 실행되지 않으므로
    // 여기서 전부 명시적으로 리셋해야 한다. 특히 waitingTask를 안
    // 지우면 슬랩 재사용으로 이전 점유자의 낡은 포인터가 남아, 리액터가
    // 완료 시 엉뚱한(이미 해제됐을 수도 있는) Task를 깨우려 든다.
    waitingTask = WeakPtr<Task>();
    submitterTask = WeakPtr<Task>();
    autoFree = true;
    cancelSource = AsyncTokenSource{};
    homeCoreIndex = Scheduler::currentCoreIndex();
    allowCoreMigration = false;
    coroHandle = nullptr;
    weakRef = nullptr;

    void* stack = GenericSlabAllocator::alloc(kAsyncTaskStackSize);
    if (!stack) {
        stackBase = 0;
        savedRsp = 0;
        return;  // 호출부(submit)가 stackBase==0을 확인해 실패 처리해야 한다
    }
    stackBase = reinterpret_cast<uint64_t>(stack);

    // kContextSwitch의 pop 순서(r15,r14,r13,r12,rbx,rbp,popfq,ret)와
    // 정확히 대응하도록 스택을 구성한다 - task.cpp의 Task::init()과
    // 완전히 동일한 레이아웃, entry만 kAsyncTaskEntryWrapper로 고정하고
    // arg는 이 AsyncTask 자신(this)이다.
    const uint64_t stackTop = stackBase + kAsyncTaskStackSize;
    auto* sp = reinterpret_cast<uint64_t*>(stackTop);
    *(--sp) = reinterpret_cast<uint64_t>(&kTaskStartTrampoline);
    *(--sp) = 0x202;                                            // RFLAGS: IF=1
    *(--sp) = 0;                                                // rbp
    *(--sp) = reinterpret_cast<uint64_t>(&kAsyncTaskEntryWrapper);  // rbx -> 트램폴린이 call
    *(--sp) = reinterpret_cast<uint64_t>(this);                 // r12 -> 트램폴린이 rdi로 옮김
    *(--sp) = 0;                                                // r13
    *(--sp) = 0;                                                // r14
    *(--sp) = 0;                                                // r15

    savedRsp = reinterpret_cast<uint64_t>(sp);
}

void AsyncTask::yield() {
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    AsyncTask* self = gCurrentAsyncTask[coreIndex];
    if (!self) {
        return;  // 리액터 컨텍스트에서(즉 AsyncTask 밖에서) 잘못 호출된 경우
    }
    if (self->state == AsyncTaskState::Running) {
        self->state = AsyncTaskState::Suspended;
    }
    // AsyncTask는 스케줄러 틱 기반 선점 대상이 아니다(리액터가 이
    // AsyncTask를 실행 중인 동안, 바깥의 kernel::Task 수준에서는
    // 여전히 "리액터 Task가 실행 중"이므로 그 틱 선점은 정상적으로
    // 별개로 계속 작동한다 - kContextSwitch가 RSP만 넘나드는 값이라
    // 어느 스택 위에서 틱이 발생하든 저장/복원이 그대로 성립한다).
    // 그래서 여기서는 cli 없이 그냥 전환해도 안전하다 - onTick/yieldCurrent
    // 쪽의 "같은 Task가 큐와 currentTask에 동시에 존재" 경쟁과 달리,
    // submitCompletion은 이 AsyncTask 자체를 currentTask 여부로 분기하지
    // 않고 그냥 큐에 넣기만 하므로 이중 스케줄링 경로가 없다.
    kContextSwitch(&self->savedRsp, gReactorSavedRsp[coreIndex]);
    // 리액터가 이 AsyncTask를 다시 뽑아 재개하면 이 지점으로 돌아온다.
}

AsyncTask* AsyncTask::submit(AsyncTaskSubjectCode subjectCode, AsyncTaskManageCode manageCode, void* args,
                              bool autoFree, bool preemptive) {
    void* mem = GenericSlabAllocator::alloc(sizeof(AsyncTask));
    if (!mem) {
        return nullptr;
    }
    // [수정, 2026-09-17, PN-9CC66142 실측 중 발견] `submitterTask`
    // (WeakPtr<Task>) 추가 후 실제 QEMU 실행에서 처음 잡힌 실측 버그 -
    // 이 슬랩 메모리는 GenericSlabAllocator의 프리리스트 칸으로 쓰인
    // 적이 있어(고전적인 침습적 프리리스트 - 빈 슬롯 자신의 메모리에
    // "다음 빈 슬롯" 포인터를 저장) `init()`이 부르기 전엔 내용이
    // 전혀 0이라는 보장이 없다. `AsyncTask::init()`의 `waitingTask =
    // WeakPtr<Task>();`/`submitterTask = WeakPtr<Task>();`는 대입
    // 연산자라 **먼저 옛 `_block`을 읽어(0이 아니면 releaseWeak()까지
    // 호출)** 그 값이 진짜 있던 값인 척 처리한다 - 프리리스트 잔여
    // 포인터를 컨트롤 블록 포인터로 오인해 그대로 역참조하면 그 값이
    // 비정규(non-canonical) 주소일 때 즉시 #GP로 죽는다(실측: 첫
    // AsyncTask::submit() 호출 - 아직 어떤 AsyncTask도 반납된 적 없는
    // "완전히 새 슬롯"인데도 재현됨 - 슬랩이 미리 여러 빈 슬롯을 한
    // 청크로 확보해 두면서 그 프리리스트 사슬을 미리 깔아 두기
    // 때문으로 보인다). `UserThread::allocate()`/`Process::allocate()`
    // 가 이미 쓰고 있는 것과 정확히 같은 해법(memset(0) 먼저) - `init()`
    // 자신은 그 뒤 모든 필드를 무조건 덮어쓰므로 이 메모리는 memset
    // 직후에도 여전히 "raw"일 뿐이고, 그 상태에서 WeakPtr 대입이
    // `_block==nullptr`을 보고 안전하게 스킵하게 만드는 게 목적이다.
    memset(mem, 0, sizeof(AsyncTask));
    auto* task = reinterpret_cast<AsyncTask*>(mem);
    task->init(subjectCode, manageCode, args);
    if (!task->stackBase) {
        GenericSlabAllocator::free(mem, sizeof(AsyncTask));
        return nullptr;
    }
    // autoFree는 init()이 리셋한 뒤, 리액터에 보여 완료될 수 있게 되기
    // 전에(submitCompletion 호출 전에) 반드시 설정해야 한다 - 그렇지
    // 않으면 아주 빨리 완료되는 작업이 기본값(true)으로 자동 반납될
    // 수 있다(경쟁).
    task->autoFree = autoFree;
    AsyncReactor::submitCompletion(task, preemptive);
    return task;
}

namespace {

bool kIsAsyncTaskTerminal(AsyncTaskState state) {
    return state == AsyncTaskState::Completed || state == AsyncTaskState::Failed ||
           state == AsyncTaskState::Cancelled;
}

}  // namespace

void AsyncTaskGroup::add(AsyncTask* task) {
    _tasks.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    _tasks.insert(task);
}

uint32_t AsyncTaskGroup::pendingCount() {
    uint32_t pending = 0;
    // forEach 도중 erase()는 안전하다 - 그 슬롯의 used를 false로
    // 내릴 뿐 순회 중인 청크/인덱스 구조 자체는 바뀌지 않는다.
    _tasks.forEach([&](AsyncTask*& task, ChunkedList<AsyncTask*, kChunkCapacity>::Slot* slot) {
        if (kIsAsyncTaskTerminal(task->state)) {
            kReleaseAsyncTask(task);
            _tasks.erase(slot);
        } else {
            ++pending;
        }
    });
    return pending;
}

void AsyncTaskWaitGroup::waitAll() {
    // **실측으로 발견한 버그(2026-09-16) 수정**: Scheduler::yieldCurrent()
    // 만으로는 리액터가 절대 실행될 기회를 못 얻는다 - runLoop()은
    // pickNext()가 뭔가를 찾는 한(이 호출 자신이 yieldCurrent()로 막
    // 다시 큐에 넣은 그 Task 자신을 즉시 재선택하는 경우 포함) 리액터
    // 드레인(idle 분기)으로 절대 안 넘어간다 - 이 Task가 유일한 Ready
    // Task이면 yieldCurrent()가 자기 자신에게 즉시 되돌아오는 무한
    // 루프가 된다(다른 Ready Task가 있을 때만 우연히 그 사이 idle이
    // 낄 여지가 생긴다 - 신뢰할 수 없는 전제). §4 (C) 경로가 이미
    // 증명한 대로 drainOnce()는 idle 컨텍스트 전용이 아니라 어떤
    // 실행 흐름에서 불러도 안전하므로, 직접 능동적으로 드레인을
    // 시도하고 - 정말 할 일이 없을 때만(false) yieldCurrent()로 다른
    // Task에게 양보한다.
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    while (_group.pendingCount() > 0) {
        if (!AsyncReactor::drainOnce(coreIndex)) {
            Scheduler::yieldCurrent();
        }
    }
}

void AsyncTaskAwaiter::await() {
    // **실측으로 발견한 버그(2026-09-16) 수정**: AsyncTask::yield()는
    // 그저 리액터 쪽으로 제어를 돌려줄 뿐, 그 뒤 아무도 다시 이
    // AsyncTask를 실행 큐에 넣어 주지 않는 한(예: Channel 핸드셰이크
    // 처럼 상대편이 명시적으로 submitCompletion() 해 주는 협조적
    // 관계) 영원히 Suspended 상태로 멈춰 있는다 - onCancel/유저
    // 소유자 종료와 무관한 "임의의 관계없는 AsyncTask 하나를 그냥
    // 기다리는" 이 범용 Awaiter에는 그런 협조자가 없어, §3.3 원안
    // 그대로("yield 반복") 구현하면 첫 yield() 이후 영원히 멈춘다
    // (실측 확인). 그래서 매번 양보하기 직전 스스로를 다시 제출해
    // (drainOnce()가 이미 지원하는 "Suspended면 Running으로 되돌려
    // 재개" 경로를 그대로 재사용) 다음 드레인 차례에 반드시 다시
    // 뽑히도록 만든다 - AsyncTask 구조체에 새 필드를 추가하지 않고도
    // (QU-86DD998F 답변 그대로) 이 AsyncTask 자신의 기존 재개
    // 메커니즘만으로 해결된다.
    const uint32_t coreIndex = Scheduler::currentCoreIndex();
    while (!kIsAsyncTaskTerminal(_target->state)) {
        AsyncTask* self = gCurrentAsyncTask[coreIndex];
        if (self) {
            AsyncReactor::submitCompletion(self);
        }
        AsyncTask::yield();
    }
    kReleaseAsyncTask(_target);
}

AsyncTaskSubjectCode AsyncCallbackRegistry::registerHandler(AsyncTaskHandler* handler) {
    SpinlockGuard guard(gRegistryLock);
    if (gNextSubjectCode >= kMaxHandlers) {
        return kMaxHandlers;  // 상한 초과 - 호출부가 resolve()로 재확인하면 항상 nullptr
    }
    const AsyncTaskSubjectCode code = gNextSubjectCode++;
    gHandlers[code] = handler;
    return code;
}

AsyncTaskHandler* AsyncCallbackRegistry::resolve(AsyncTaskSubjectCode subjectCode) {
    if (subjectCode >= kMaxHandlers) {
        return nullptr;
    }
    return gHandlers[subjectCode];
}

void AsyncReactor::init() {
    // 전역 IDT 등록이라 BSP에서 한 번만(tlb_shootdown.cpp와 동일한
    // 이유) - AP는 이 클래스를 위해 더 이상 아무것도 부를 필요가 없다
    // (코어별 큐는 이미 정적 배열, 전용 Task 자체가 없어졌다).
    Idt::registerHandler(kAsyncDrainVector, kAsyncDrainIsr);
}

bool AsyncReactor::drainOnce(uint32_t coreIndex) {
    // [신규, 2026-09-17, PN-495C11B7, SP-B1E258D8 §5.3] RCU call_rcu
    // 콜백 드레인 - "콜백 실행은 반드시 리액터 컨텍스트에서"라는
    // §3.5 인터럽트 통합 규칙 그대로, 이 함수가 실제 리액터 드레인
    // 지점이므로 AsyncTask 재진입 가드(gDraining, 아래)와 무관하게
    // 매 호출마다 먼저 확인한다(AsyncTask 코루틴 재개 상태를 전혀
    // 건드리지 않아 재진입 중에도 안전).
    Rcu::drainCallbacksOnThisCore();

    if (gDraining[coreIndex]) {
        // 이미 이 코어에서(runLoop() 인라인 호출이든 §4 (C) IPI
        // 핸들러든) 드레인이 진행 중 - gReactorSavedRsp[coreIndex]를
        // 두 번 건드리면 진행 중인 AsyncTask의 재개 지점이 깨진다.
        // 새로 큐잉된 항목은 바깥쪽 호출이 이어서 처리하므로 유실
        // 걱정 없다.
        return false;
    }

    // 선점 큐(§2.2)를 항상 먼저 확인한다 - 비어 있으면 일반 큐로.
    AsyncTask* task = gPreemptiveQueues[coreIndex].popFront();
    if (!task) {
        task = gExecQueues[coreIndex].popFront();
    }
    if (!task) {
        // 지연 실행 큐(SP-F15B4A63, PN-C46DF296) - 전용 커널 Task를
        // 새로 만들지 않고 이 코어의 idle 분기에서 만료 타이머를
        // 처리한다(§3, QU-A8C0CC2C 설계자 답변). pump()가 만료된
        // 콜백을 실행하는 도중 AsyncTask::submit()으로 새 작업을 큐에
        // 넣을 수 있으므로, 포기하기 전에 먼저 실행 큐를 한 번 더
        // 확인한다.
        DelayedExecutionQueue::pump();
        task = gPreemptiveQueues[coreIndex].popFront();
        if (!task) {
            task = gExecQueues[coreIndex].popFront();
        }
    }
    if (!task) {
        // 정말 아무 것도 없다 - 호출부(runLoop()의 idle 분기)가 그대로
        // hlt로 진행한다. 아직 만료 안 된 타이머가 남아 있어도 별도
        // 조치가 필요 없다(2026-09-16 재구조로 해소) - 스케줄러 틱이
        // 이미 100Hz로 모든 idle 코어를 hlt에서 깨우므로, 다음 틱에서
        // 이 함수가 다시 호출되면 그때 다시 확인된다.
        return false;
    }

    gDraining[coreIndex] = true;

    if (task->state == AsyncTaskState::Cancelled) {
        // [PN-40E976F2] 원래 시나리오 - 이 AsyncTask를 기다리던
        // UserThread가 이미 죽어(scheduler.cpp의 SelfTerminateHandler::
        // onExec) 결과를 가져갈 사람이 없다 - onExec을 실행/재개하지
        // 않고 곧장 onCancel만 부른 뒤 자원을 반납한다. autoFree는
        // 그 경우 취소 시점에 이미 강제로 true가 돼 있다.
        AsyncTaskHandler* handler = AsyncCallbackRegistry::resolve(task->subjectCode);
        if (handler) {
            handler->onCancel(task, task->args);
        }
        // [신규, 2026-09-18, PN-B5C2845A] 새 시나리오 - 이 AsyncTask의
        // 원래 제출자가 아직 살아서 `Syscall::wait()`로 이 토큰을
        // 기다리며 파킹돼 있을 수 있다(다른 프로세스의 Kill이
        // `Scheduler::cancelPendingSyscalls()`로 이 상태를 만든 경우 -
        // PN-40E976F2의 "제출자가 이미 죽었다" 전제가 더 이상 항상
        // 참이 아니게 됨). 그 경우 `autoFree`는 false로 남아 있으므로
        // (cancelPendingSyscalls 문서 참고) 아래에서 반납하지 않고,
        // 대기자를 먼저 깨워 `Syscall::waitForAnyOf()`가 스스로
        // 소비/반납하게 한다 - `Completed`/`Failed` 분기(아래)가 이미
        // 하는 것과 동일한 패턴.
        if (SharedPtr<Task> waiter = task->waitingTask.lock()) {
            Scheduler::scheduleImmediate(coreIndex, waiter.get());
        }
        if (task->autoFree) {
            kReleaseAsyncTask(task);
        }
        gDraining[coreIndex] = false;
        return true;
    }

    if (task->coroHandle) {
        // [PN-C62F7908 4/5, SP-F682B889 §7.3] 이전에 코루틴이 co_await로
        // suspend된 채 남아 있다 - 이 AsyncTask의 전용 스택(kContextSwitch)
        // 은 kAsyncTaskEntryWrapper가 첫 suspend에서 이미 마지막으로 썼다
        // (그 함수 주석 참고) - 이후로는 다시 그 스택으로 돌아가지 않고
        // `coroutine_handle::resume()`만으로 재개한다(리액터 자신의
        // 스택 위에서 직접 실행되는 일반 함수 호출 - 별도 스택 전환
        // 없음, 코루틴 방식이 스택풀 방식보다 가벼운 핵심 이유).
        task->state = AsyncTaskState::Running;
        gCurrentAsyncTask[coreIndex] = task;
        {
            // 아래 스택풀 경로와 동일한 이유로 이 구간도 Task 수준
            // 선점 대상에서 제외한다(같은 "AsyncTask가 실행 슬롯을
            // 빌려 쓰는 동안" 불변조건 - 재개 메커니즘만 다를 뿐 이
            // 구간이 "AsyncTask가 실행 중"이라는 의미 자체는 동일).
            PreemptionGuard guard;
            task->coroHandle.resume();
        }
        gCurrentAsyncTask[coreIndex] = nullptr;

        if (task->coroHandle.done()) {
            task->coroHandle.destroy();
            task->coroHandle = nullptr;
            task->state = AsyncTaskState::Completed;
        } else {
            // 다시 co_await로 suspend됨 - coroHandle을 그대로 남겨
            // 다음 drainOnce()가 다시 이 경로를 고르게 한다.
            task->state = AsyncTaskState::Suspended;
        }
    } else {
        if (task->state == AsyncTaskState::Ready || task->state == AsyncTaskState::Suspended) {
            task->state = AsyncTaskState::Running;
        }
        gCurrentAsyncTask[coreIndex] = task;
        {
            // AsyncTask 프레임워크의 원래 설계 의도(kAsyncTaskEntryWrapper
            // 주석 참고 - "AsyncTask를 실행하는 동안 바깥 kernel::Task
            // 수준에서는 여전히 (예전 리액터에 해당하는) 그 흐름이 실행
            // 중이어야 한다")를 실제로 강제한다 - 이 구간(AsyncTask가 이
            // 실행 슬롯을 "빌려 쓰는" 동안) 전체를 Task 수준 선점 대상에서
            // 제외한다(Slab 매거진 보호에 쓰는 것과 같은 PreemptionGuard
            // 재사용 - 인터럽트 자체는 막지 않아 EOI/하드웨어 처리는 정상
            // 진행됨).
            PreemptionGuard guard;
            kContextSwitch(&gReactorSavedRsp[coreIndex], task->savedRsp);
        }
        gCurrentAsyncTask[coreIndex] = nullptr;
    }

    if (task->state == AsyncTaskState::Completed || task->state == AsyncTaskState::Failed) {
        // 이 AsyncTask가 끝나기를 기다리는 kernel::Task가 있으면
        // (예: Syscall::wait) 먼저 깨운다 - 이 코어에서 실행됐으니
        // 대기자도 반드시 같은 코어에서 파킹돼 있다(v1 - 코어 간
        // 이관 없음). 아래에서 task를 반납하기 전에 반드시 먼저
        // 읽어야 한다(반납 후에는 이 필드도 더 이상 유효하지 않음).
        // [수정, 2026-09-17, PN-B4987BF6] `waitingTask`가 이제
        // `WeakPtr<Task>`라 `.lock()`으로 유효성을 확인해야 한다 - 대상
        // UserThread가 이 AsyncTask 완료 전에 강제 종료돼 이미
        // release()됐으면 조용히 스킵한다(async_task.h의 waitingTask
        // 주석 참고).
        if (SharedPtr<Task> waiter = task->waitingTask.lock()) {
            Scheduler::scheduleImmediate(coreIndex, waiter.get());
        }
        // args의 생성/반납은 처리기 책임(SP-F682B889 §3.1) - 여기서는
        // 프레임워크 소유물(AsyncTask 구조체 자신과 그 전용 스택)만,
        // 그것도 autoFree인 경우에만 반납한다 - false면 결과를 아직
        // 못 읽은 소비자(위에서 막 깨운 그 Task)가 직접 반납할
        // 책임을 진다.
        if (task->autoFree) {
            kReleaseAsyncTask(task);
        }
    }
    // Suspended면 아무 것도 안 함 - 나중에 submitCompletion으로 다시
    // 큐에 들어와야 재개된다.
    gDraining[coreIndex] = false;
    return true;
}

void AsyncReactor::submitCompletion(AsyncTask* task, bool preemptive) {
    // [수정, 2026-09-16, PN-622BA93C/QU-FDB32CCE] 큐잉 대상은 호출자
    // 자신의 현재 코어가 아니라 이 task의 홈 코어다(allowCoreMigration
    // 옵트인 시에만 예외) - async_task.h의 submitCompletion 주석 참고.
    // 이전에는 항상 호출자 자신의 코어에 큐잉해, 서로 다른 코어에서
    // 실행 중인 두 AsyncTask가 completion을 주고받는 협조적 패턴
    // (channel.cpp의 accepter/connector 핸드셰이크 등)에서 target을
    // 엉뚱한 코어의 큐에 넣어버리는 실측 버그가 있었다.
    const uint32_t targetCore = task->allowCoreMigration ? Scheduler::currentCoreIndex() : task->homeCoreIndex;
    if (preemptive) {
        gPreemptiveQueues[targetCore].pushBack(task);
    } else {
        gExecQueues[targetCore].pushBack(task);
    }
    // [재설계, 2026-09-16, QU-3BDEE348 답변 - 하이브리드 (A)+(C)]
    // 리액터가 더 이상 Task가 아니므로(async_task.h 주석 참고) "파킹돼
    // 있으면 강제 스케줄링" 판단 자체가 사라졌다 - preemptive==false
    // (A)는 조치 없이 그냥 반환(그 코어가 다음 idle 분기에서 자연히
    // 처리), preemptive==true(C)만 targetCore에게 kAsyncDrainVector
    // IPI를 보내 그 코어의 인터럽트가 다시 켜지는 즉시 drainOnce()가
    // 반복 호출되게 한다. 이 함수는 인터럽트 컨텍스트에서도 호출
    // 가능하다고 문서화돼 있어(async_task.h) IPI 발사 자체는 안전하다
    // (targetCore가 호출자 자신이 아닐 수도 있게 된 뒤에도 여전히
    // 안전 - Lapic::sendFixedIpi()는 그저 ICR에 쓰는 것뿐이고, 다른
    // 코어를 깨우는 이 패턴 자체는 scheduler.cpp의 Push/Pull
    // 로드밸런싱 kWakeCoreIfIdle이 이미 같은 방식으로 쓰고 있다).
    if (preemptive) {
        Lapic::sendFixedIpi(Acpi::cpuApicId(targetCore), static_cast<uint8_t>(kAsyncDrainVector));
    }
}

}  // namespace kernel
