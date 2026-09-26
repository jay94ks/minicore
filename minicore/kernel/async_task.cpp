#include "async_task.h"

#include "acpi.h"
#include "deferred_destruction.h"
#include "delayed_exec.h"
#include "diag_ring.h"
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

// [신규, 2026-09-24, PN-E4C6AF72 §"남은 것" 0번] StackfulDispatchBegin/End
// 시점엔 InterruptFrame이 없다(호출 컨텍스트가 인터럽트 스택이 아닐 수도
// 있음 - kAsyncDrainIsr/self-IPI 경유든 waitAll()의 Task 레벨 호출이든
// 공통으로 쓰는 지점이라서) - 그래서 스택에 실려 온 값을 읽는 대신
// 지금 이 순간의 실제 CS 레지스터 값을 직접 읽는다.
kernel::uint64_t kReadCurrentCodeSegment() {
    kernel::uint16_t cs = 0;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

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
// 경로의 IPI 핸들러든)가 AsyncTask로 전환하기 직전의 자기 자신
// TaskTcb* - AsyncTask::yield()가 돌아올 자리(옛 "리액터 Task 컨텍스트"와
// 정확히 같은 역할, 이름만 유지 - 별도 Task가 아니게 됐다고 해서 이
// 슬롯 자체의 의미가 바뀌지는 않는다). [갱신, 2026-09-20, PN-81E49523
// 2단계] `uint64_t`에서 `TaskTcb*`로 - kContextSwitch에 변환 없이
// 바로 넘기기 위함.
//
// [수정, 2026-09-20, PN-81E49523 2단계 - minicore-3c 교차 진단 +
// 실측으로 확인] 이 배열은 예전엔 `uint64_t`(RSP 값 자체)라 그냥
// 0에서 시작해도 안전했다(옛 kContextSwitch가 push한 뒤의 RSP를
// 대입만 했다) - 그런데 `TaskTcb*`로 바뀌면서 새 kContextSwitch의
// 저장 절반이 "이 포인터가 가리키는 곳에 필드를 직접 쓴다"로 바뀌어,
// 이 포인터 자신이 미리 유효한 버퍼를 가리키고 있어야 하는데 아무도
// 채워 준 적이 없었다(Task::tcb/AsyncTask::tcb가 각자 init()에서
// slab 할당을 받는 것과 달리, 이 전역은 그런 초기화 지점이 아예
// 없었다) - 부팅 후 첫 `AsyncReactor::drainOnce()`가 §4(C) 경로로
// 반드시 한 번은 도달하는 시점(4개 서비스 스폰 직후 첫 IPI 강제
// 드레인)에 `*(TaskTcb*)nullptr`에 레지스터를 쓰다 크래시했다(cr2=
// 0x8=offsetof(TaskTcb,rbx), 이전 세션이 보고한 그 신호 그대로) -
// `gIdleTaskTcb`/`gDiscardedBootTcb`와 같은 패턴으로 전용 정적 버퍼를
// 만들고 `AsyncReactor::init()`에서 한 번에 연결한다.
kernel::TaskTcb gReactorTcbStorage[kMaxCores];
kernel::TaskTcb* gReactorSavedRsp[kMaxCores] = {};

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

// [신규, 2026-09-25, PN-4859FDE9, QU-F90FB07F 답변("배치 상한 + 자기
// IPI 재예약")] 이전 버전은 `drainOnce()`가 false를 반환할 때까지 무제한
// 루프를 돌았다 - AHCI 커맨드 완료 처리 중 같은 코어의 같은 큐에 다음
// 커맨드를 즉시 `submit()`하는 패턴(스왑 I/O 등)을 만나면 외부 IPI 없이도
// 이 루프가 영원히 새 작업을 찾아 계속 돌고, 이 ISR이 IF=0 상태로
// 반환하지 않아 스케줄러 LAPIC 타이머 틱을 포함한 모든 일반 인터럽트가
// 150초+ 굶는 것을 실측으로 확인했다(async_task.cpp 문서 이력, PN-4859FDE9
// 참고). 한 번의 ISR 호출이 처리하는 작업 수를 이 상한으로 제한하고,
// 상한에 도달했는데 아직 처리할 게 남아 있을 수 있으면(마지막 호출이
// true를 반환) 스스로에게 같은 벡터로 IPI를 재발사한 뒤 그냥 반환한다 -
// 이 ISR이 실제로 반환해야 iretq가 IF를 복원하고, 그 순간 대기 중인 다른
// 일반 인터럽트(타이머 틱 포함)가 끼어들 기회를 얻는다(재발사한 self-IPI
// 자신도 그중 하나로 대기하다가 곧 처리됨 - 큐가 실제로 비어 있었다면
// 다음 호출의 drainOnce() 한 번이 즉시 false를 반환하고 끝나므로 낭비가
// 거의 없다). 값 자체(32)는 이 프로젝트가 아직 실측 튜닝을 해 본 적
// 없는 v1 추정치 - AHCI NCQ 큐 깊이(전형적으로 32슬롯) 정도를 한 배치로
// 다 처리해도 ISR 체류 시간이 과도해지지 않을 것이라는 보수적 가정.
// 실측으로 너무 크거나 작다고 드러나면 조정 대상(RM-23F4B687 §4 취지상
// 숫자값 수준은 구현 중 결정 가능한 범위).
constexpr kernel::uint32_t kAsyncDrainBatchLimit = 32;

void kAsyncDrainIsr(kernel::InterruptFrame*) {
    const kernel::uint32_t coreIndex = kernel::Scheduler::currentCoreIndex();
    kernel::uint32_t processed = 0;
    while (kernel::AsyncReactor::drainOnce(coreIndex)) {
        if (++processed >= kAsyncDrainBatchLimit) {
            kernel::kDiagRingLog(kernel::DiagRingEvent::AsyncDrainBatchLimitHit, coreIndex, processed, 0, coreIndex);
            kernel::Lapic::sendFixedIpi(kernel::Acpi::cpuApicId(coreIndex), static_cast<kernel::uint8_t>(kAsyncDrainVector));
            return;
        }
    }
}

// [갱신, SP-39F18E30 §2, AllocDmaBuffer/FreeDmaBuffer 추가] 64는 정확히
// 그 시점까지 등록된 핸들러 총수와 같아 꽉 찬 상한이었다 - 그 사실을
// 몰랐던 채로 새 syscall 2개를 등록하면(RM-48E1E610 그룹2 call 2/3)
// registerHandler()가 상한 초과를 조용히 kMaxHandlers로만 반환하고,
// 그 뒤에 등록되는 핸들러(당시 순서상 DebugSession 등)가 슬롯을 못 받아
// 나중에 그 subjectCode로 조회하면 nullptr - 실측으로 "#DB unhandled"
// 경고 뒤 페이지 폴트 패닉으로 발견했다. 128로 넉넉히 올려 둔다(v1
// 상한 - 필요해지면 다시 늘림).
constexpr kernel::uint32_t kMaxHandlers = 128;
kernel::AsyncTaskHandler* gHandlers[kMaxHandlers] = {};
kernel::uint32_t gNextSubjectCode = 0;
kernel::Spinlock gRegistryLock;

}  // namespace

namespace kernel {

// [갱신, 2026-09-18, SP-76250478 §3.1, PN-0EB2FABF] `AsyncTaskWeakRef`
// 클래스 정의 자체는 이제 async_task.h에 있다(async_task.cpp 전용
// 비공개 구현에서 헤더의 공개 재사용 primitive로 승격 - 그 헤더의
// 클래스 문서 주석 참고, `Join` syscall이 두 번째 소비자가 됐다).

AsyncTaskWeakRef* AsyncTask::ensureWeakRef() {
    if (weakRef) {
        return weakRef;  // 이미 있으면 그대로 재사용(멱등)
    }
    void* mem = GenericSlabAllocator::alloc(sizeof(AsyncTaskWeakRef));
    if (!mem) {
        return nullptr;  // 할당 실패
    }
    weakRef = reinterpret_cast<AsyncTaskWeakRef*>(mem);
    weakRef->init(this);
    return weakRef;
}

// [신규, 2026-09-19, PN-0AC554C2/PN-EA968DF0, QU-B89531F0 답변]
// `AsyncTaskWaitable`은 `Waitable`을 상속해 vtable을 갖는다 -
// `kMakeShared`의 기본 관례(memset(0)+init(), 실제 생성자 안 거침)로는
// vtable 포인터가 설치되지 않아 가상 호출이 즉시 크래시한다(shared_ptr.h
// 의 "가상 함수가 있는 T" 경고, QU-1D089097이 WaitQueue-in-Mutex에서
// 이미 실측한 것과 같은 함정) - 그래서 반드시 `kMakeSharedNew`(실제
// placement new 생성자 호출)를 써야 한다.
SharedPtr<AsyncTaskWaitable> AsyncTask::ensureWaitable() {
    if (selfWaitable) {
        return selfWaitable;  // 이미 있으면 그대로 재사용(멱등)
    }
    AsyncTaskWeakRef* ref = ensureWeakRef();
    if (!ref) {
        return SharedPtr<AsyncTaskWaitable>();  // 할당 실패
    }
    selfWaitable = kMakeSharedNew<AsyncTaskWaitable>(ref);
    return selfWaitable;
}

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
    if (timeoutScheduled) {
        return;  // v1 - 이미 걸려 있으면 두 번째 호출은 무시(설계 문서 그대로)
    }
    AsyncTaskWeakRef* ref = ensureWeakRef();
    if (!ref) {
        return;  // 할당 실패 - 타임아웃 없이 계속 진행(치명적이지 않음)
    }
    timeoutScheduled = true;
    ref->addRef();  // 타이머 콜백 쪽 몫
    DelayedExecutionQueue::schedule(delayTicks, &kOnAsyncTaskTimeout, ref);
}

// [PN-D01B7D07] 이 AsyncTask를 실제로 반납하는 유일한 통로 - 기존에
// 4곳(AsyncTaskGroup::pendingCount()/AsyncTaskAwaiter::~AsyncTaskAwaiter/
// drainOnce()의 Cancelled/Completed·Failed 분기)에 각자 따로 있던
// "스택 반납 + 구조체 반납" 두 줄을 여기로 모았다 - weakRef 무효화를
// **정확히 한 곳**에서만 하면 되게 하기 위함(그렇지 않으면 4곳
// 전부에 빠짐없이 배선해야 해 QU-0CB8CAEE가 지적한 "새 필드 최소화"
// 취지와 다시 부딪힌다).
void kReleaseAsyncTask(AsyncTask* task) {
    // [신규, 2026-09-18, SP-76250478 §3.1, PN-0EB2FABF 조사 중 발견]
    // **잠재 버그 수정** - `drainOnce()`의 정상 완료 경로(coroHandle.
    // done()==true)는 이미 여기 도달하기 전에 `coroHandle.destroy()`+
    // `coroHandle=nullptr`을 직접 해서 이 시점엔 항상 비어 있었지만,
    // `state==Cancelled` 경로(drainOnce() 상단, `Scheduler::
    // cancelPendingSyscalls()`가 만드는 경로)는 코루틴이 **suspend된
    // 채로** 취소될 수 있는데도 coroHandle을 전혀 안 건드리고 곧장
    // 이 함수로 넘어왔다 - 그러면 코루틴 프레임(및 그 안의 지역
    // SharedPtr/WeakPtr 등 - 소멸자가 안 불림) 자체가 영원히 누수됐다.
    // `Join`(§3.1) 착수 전까지는 이 커널의 어떤 onExec()도 실제로
    // `co_await`로 suspend된 적이 없어(전부 co_return으로 즉시 종료)
    // 이 경로 자체가 한 번도 실행된 적이 없었던 잠재 버그 - Join의
    // "대상이 아직 안 끝났으면 정지" 경로가 이 프레임워크의 첫 실제
    // suspend 지점이라 여기서 처음 발견/수정한다.
    if (task->coroHandle) {
        task->coroHandle.destroy();
        task->coroHandle = nullptr;
    }
    if (task->weakRef) {
        task->weakRef->invalidate();
        task->weakRef->release();
        task->weakRef = nullptr;
    }
    // [신규, 2026-09-22, PN-6EDED542] `waitingAsyncTask`는 정상 경로면
    // drainOnce()가 완료 처리 중에 이미 소비(release())하고 nullptr로
    // 비웠어야 한다 - 여기 남아 있다는 건 그 소비 지점을 거치지 않고
    // (예: Cancelled 경로가 훗날 놓치는 경우) 이 함수로 곧장 온
    // 예외적 상황이라는 뜻이다. `weakRef`와 동일한 이유로 방치하면
    // 그 대기자 쪽 참조 카운트가 영원히 안 내려간다 - 방어적으로
    // 여기서도 반드시 정리한다.
    if (task->waitingAsyncTask) {
        task->waitingAsyncTask->release();
        task->waitingAsyncTask = nullptr;
    }
    // [신규, 2026-09-22, PN-2954EC4D] `waitingTask`(WeakPtr<Task>)/
    // `submitterTask`(TaskOwnerRef, 내부에 WeakPtr<Task> 보유)/
    // `selfWaitable`(SharedPtr<AsyncTaskWaitable>)을 명시적으로
    // 비운다 - 이 프로젝트의 `T::destroy()`류 관례(실제 소멸자를
    // 절대 안 부름)와 동일한 이유로, 이 대입들(각 operator=가 옛
    // 컨트롤 블록에 releaseWeak()/release Strong을 호출) 없이 그냥
    // 슬랩을 반납하면 이 필드들이 가리키던 대상의 컨트롤 블록
    // 참조 카운트가 영원히 안 내려간다 - `submitterTask`는 사실상
    // 모든 syscall이 만드는 AsyncTask마다 채워지므로(channel.cpp의
    // BridgePipe::peer보다 훨씬 넓은 반경) 방치하면 가장 흔하게
    // 새는 경로가 된다(코드 감사로 발견, 아직 실측 계측 없음).
    // `AsyncTask::init()`이 재사용 슬롯을 위해 이미 동일한 대입을
    // 하고 있는 것과 정확히 대칭 - 그쪽은 "다음 사용자를 위한 초기화"
    // 목적이고 이쪽은 "이번 사용자의 마지막 정리" 목적이다.
    task->waitingTask = WeakPtr<Task>();
    task->submitterTask = TaskOwnerRef();
    task->selfWaitable.reset();
    GenericSlabAllocator::free(reinterpret_cast<void*>(task->stackBase), kAsyncTaskStackSize);
    if (task->tcb) {
        // [신규, 2026-09-20, PN-81E49523 2단계] tcb가 이제 stackBase와
        // 완전히 별도의 Slab 할당 - AsyncTask 자신을 반납하기 전에
        // 먼저 반납해야 새지 않는다.
        GenericSlabAllocator::free(task->tcb, sizeof(TaskTcb));
    }
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
    submitterTask = TaskOwnerRef();  // [갱신, PN-C536F352] WeakPtr<Task> -> TaskOwnerRef, 아래 memset(0) 전제는 그대로 유지
    waitingAsyncTask = nullptr;  // [PN-6EDED542] waitingTask와 동일한 이유(슬랩 재사용 잔여 포인터 방지)
    autoFree = true;
    cancelSource = AsyncTokenSource{};
    homeCoreIndex = Scheduler::currentCoreIndex();
    allowCoreMigration = false;
    coroHandle = nullptr;
    weakRef = nullptr;
    selfWaitable = SharedPtr<AsyncTaskWaitable>();
    timeoutScheduled = false;
    coroYieldRetryStreak = 0;  // [PN-F2594E93/DC-5F0AC0D3] 슬랩 재사용 잔여값 방지

    void* stack = GenericSlabAllocator::alloc(kAsyncTaskStackSize);
    if (!stack) {
        stackBase = 0;
        tcb = nullptr;
        return;  // 호출부(submit)가 stackBase==0을 확인해 실패 처리해야 한다
    }
    stackBase = reinterpret_cast<uint64_t>(stack);

    // [갱신, 2026-09-20, PN-81E49523 2단계, 설계자 지시] task.cpp의
    // Task::init()과 완전히 동일한 이유/레이아웃 - 이 AsyncTask 전용
    // 고정 TaskTcb 블록을 커널 스택(=이 AsyncTask 전용 실행 스택
    // `stackBase`)과 완전히 별도로 Slab에서 할당한다. entry만
    // kAsyncTaskEntryWrapper로 고정하고 arg는 이 AsyncTask 자신(this)이다.
    const uint64_t stackTop = stackBase + kAsyncTaskStackSize;
    if (tcb) {
        GenericSlabAllocator::free(tcb, sizeof(TaskTcb));
    }
    tcb = reinterpret_cast<TaskTcb*>(GenericSlabAllocator::alloc(sizeof(TaskTcb)));
    *tcb = TaskTcb{};
    tcb->rbx = reinterpret_cast<uint64_t>(&kAsyncTaskEntryWrapper);  // 트램폴린이 call
    tcb->r12 = reinterpret_cast<uint64_t>(this);                     // 트램폴린이 rdi로 옮김
    tcb->rip = reinterpret_cast<uint64_t>(&kTaskStartTrampoline);
    tcb->cs = 0x08;                                                  // kGdtKernelCodeSelector
    tcb->rflags = 0x202;                                             // IF=1
    tcb->rspOld = stackTop;
    tcb->ssOld = 0x10;                                               // kGdtKernelDataSelector
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
    kContextSwitch(&self->tcb, gReactorSavedRsp[coreIndex]);
    // 리액터가 이 AsyncTask를 다시 뽑아 재개하면 이 지점으로 돌아온다.
}

// [신규, 2026-09-22, PN-4D60D49C] async_task.h 선언 참고 - 기존
// `gCurrentAsyncTask[coreIndex]`(위 yield() 등이 이미 참조하는 그
// 배열)를 그대로 노출하는 얇은 접근자.
AsyncTask* AsyncTask::current() {
    return gCurrentAsyncTask[Scheduler::currentCoreIndex()];
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

// [신규, 2026-09-22, PN-6EDED542] `task`가 막 Completed/Failed/
// Cancelled에 도달한 시점에 호출 - `waitingTask`(진짜 kernel::Task)
// 깨우기와 나란히, 코루틴/스택풀 AsyncTask로 `co_await
// AsyncTaskCoroAwaiter(task)`(또는 `AsyncTaskAwaiter`)로 대기 중인
// 다른 AsyncTask가 있으면 `AsyncReactor::submitCompletion()`으로
// 재개시킨다 - 그쪽이 코루틴이면 저장된 coroHandle을 다음 drainOnce()
// 가 resume()하고, 스택풀이면 기존 kContextSwitch 재개 경로를 그대로
// 다시 탄다(둘 다 이 함수가 신경 쓸 필요 없음 - drainOnce() 자신의
// 기존 두 분기가 그 차이를 이미 처리).
void kWakeWaitingAsyncTask(AsyncTask* task) {
    AsyncTaskWeakRef* waiterRef = task->waitingAsyncTask;
    if (!waiterRef) {
        return;
    }
    task->waitingAsyncTask = nullptr;
    if (AsyncTask* waiter = waiterRef->lock()) {
        AsyncReactor::submitCompletion(waiter, /*preemptive=*/true);
    }
    waiterRef->release();  // AsyncTaskCoroAwaiter::await_suspend()가 심어 둔 "waitingAsyncTask" 몫
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
    // [신규, 2026-09-20, PN-81E49523 2단계] gReactorSavedRsp 문서 주석
    // 참고 - 모든 코어의 슬롯이 첫 drainOnce() 이전에 이미 유효한
    // 버퍼를 가리키도록 여기서 한 번에 연결한다(순수 정적 배열 쓰기라
    // 이 코어/저 코어 구분 없이 BSP 혼자 전부 채워도 안전).
    for (uint32_t i = 0; i < kMaxCores; ++i) {
        gReactorSavedRsp[i] = &gReactorTcbStorage[i];
    }
}

bool AsyncReactor::drainOnce(uint32_t coreIndex) {
    // [신규, 2026-09-17, PN-495C11B7, SP-B1E258D8 §5.3] RCU call_rcu
    // 콜백 드레인 - "콜백 실행은 반드시 리액터 컨텍스트에서"라는
    // §3.5 인터럽트 통합 규칙 그대로, 이 함수가 실제 리액터 드레인
    // 지점이므로 AsyncTask 재진입 가드(gDraining, 아래)와 무관하게
    // 매 호출마다 먼저 확인한다(AsyncTask 코루틴 재개 상태를 전혀
    // 건드리지 않아 재진입 중에도 안전).
    Rcu::drainCallbacksOnThisCore();

    // [신규, 2026-09-20, SP-5130284C, PN-4137C88C] 인터럽트 컨텍스트
    // (onTick()/onForcedMigration() 등)에서 SharedPtr가 마지막 강한
    // 참조를 잃어 지연됐던 무거운 소멸(예: Process::destroy())을
    // 지금(이 안전한 리액터 컨텍스트) 대신 실행한다 - 위 Rcu 드레인과
    // 동일한 이유로 gDraining 재진입 가드와 무관하게 매 호출마다 먼저
    // 확인한다.
    kDrainDeferredDestructions();

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

    // [신규, 2026-09-23, PN-E4C6AF72 3차 실측의 "남은 것" 1번] queue
    // pop이 성공적으로 끝난 직후 - vector 필드에 subjectCode를 실어
    // 어떤 종류의 AsyncTask였는지 남긴다(self-IPI로 이 함수가 불렸는데
    // 여기까지도 못 왔다면 popFront() 자체나 그 이전 Rcu/deferred
    // destruction 드레인 중 손상됐다는 뜻).
    kDiagRingLog(DiagRingEvent::DrainOnceTaskFound, coreIndex, task->subjectCode, 0);

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
        kWakeWaitingAsyncTask(task);
        if (task->autoFree) {
            kReleaseAsyncTask(task);
        }
        gDraining[coreIndex] = false;
        return true;
    }

    if (task->coroHandle) {
        // [신규, 2026-09-23, PN-E4C6AF72] 이 분기는 코루틴 재개일 뿐
        // kContextSwitch를 안 타므로 StackfulDispatchBegin이 절대 안
        // 찍히는 게 정상이다 - "self-IPI 이후 Begin이 없다"는 관측이
        // 실제로는 이 무해한 분기였을 가능성을 직접 배제/확인하기 위한
        // 계측.
        kDiagRingLog(DiagRingEvent::DrainOnceCoroBranch, coreIndex, 0, 0);
        // [PN-C62F7908 4/5, SP-F682B889 §7.3] 이전에 코루틴이 co_await로
        // suspend된 채 남아 있다 - 이 AsyncTask의 전용 스택(kContextSwitch)
        // 은 kAsyncTaskEntryWrapper가 첫 suspend에서 이미 마지막으로 썼다
        // (그 함수 주석 참고) - 이후로는 다시 그 스택으로 돌아가지 않고
        // `coroutine_handle::resume()`만으로 재개한다(리액터 자신의
        // 스택 위에서 직접 실행되는 일반 함수 호출 - 별도 스택 전환
        // 없음, 코루틴 방식이 스택풀 방식보다 가벼운 핵심 이유).
        //
        // [신규, 2026-09-18, SP-76250478 §3.1, PN-0EB2FABF 실측 발견 -
        // PN-523B779F "남은 범위" 실제 수정] `kAsyncTaskEntryWrapper`
        // 문서 주석이 이미 정확히 예고해 둔 그 CR3 미동기화 버그를
        // 여기서 처음 실제로 겪었다 - `Join`(process.cpp)이 이 코드베이스
        // 최초로 실제 `co_await` 정지+재개를 쓰는 기능이라, 재개된
        // 코루틴 본문이 (제출자의) 유저 포인터(`JoinArgs*`)를 그대로
        // 역참조하는데, 이 지점의 CR3는 재개 시점에 우연히 어떤 값이든
        // 될 수 있어(제출자의 주소공간이라는 보장이 전혀 없음) 실측으로
        // Page Fault → PANIC까지 재현됐다(devmgr TEMP 하네스, GRUB SMP4).
        // `PN-2008220B`가 이미 "위험한 부트 스택" 문제를 없애 둬(idle
        // 컨텍스트가 이제 전용 higher-half 스택) 이 지점에서 CR3를 바꿔도
        // 더 이상 안전하지 않을 이유가 없다 - `kAsyncTaskEntryWrapper`와
        // 정확히 같은 두 헬퍼(`kSyncCr3ForAsyncExecEntry`/
        // `kRestoreCr3AfterAsyncExecEntry`)를 그대로 재사용한다.
        const uint64_t savedPml4ForResume = kSyncCr3ForAsyncExecEntry(task);
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
        // [PN-0EB2FABF] kAsyncTaskEntryWrapper와 동일 - 리액터/idle
        // 컨텍스트로 돌아가기 전 CR3를 이 재개 이전 값으로 되돌린다.
        kRestoreCr3AfterAsyncExecEntry(savedPml4ForResume);

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
        // [신규, 2026-09-23, PN-E4C6AF72] StackfulDispatchBegin(아래,
        // CR3 동기화+PreemptionGuard 이후)보다 한 단계 이른 지점 - 이
        // 로그와 StackfulDispatchBegin 사이에서 사라지면 CR3 동기화
        // 자체(kSyncCr3ForAsyncExecEntry) 또는 그 직후 상태 갱신이
        // 의심 구간으로 좁혀진다.
        kDiagRingLog(DiagRingEvent::DrainOnceStackfulBranch, coreIndex, 0, 0);
        if (task->state == AsyncTaskState::Ready || task->state == AsyncTaskState::Suspended) {
            task->state = AsyncTaskState::Running;
        }
        gCurrentAsyncTask[coreIndex] = task;
        // [신규, 2026-09-19, PN-584DB994 재검증 중 실측 발견 - 위
        // coroHandle 분기가 이미 고친 것과 정확히 같은 CR3 미동기화
        // 버그의 두 번째 사례] 이 분기는 최초 진입(kTaskStartTrampoline
        // -> kAsyncTaskEntryWrapper, 그 자신이 진입 시 별도로
        // kSyncCr3ForAsyncExecEntry를 부른다)뿐 아니라, `co_await`을
        // 전혀 안 쓰고 onExec() 본문 중간에서 직접
        // `AsyncTask::yield()`를 반복 호출하는 "busy-yield" 패턴(예:
        // ConnectChannelHandler::onExec의 `while (!req.done) {
        // AsyncTask::yield(); }`, user_sync.cpp의 MutexLock/
        // SemaphoreWait - 둘 다 AsyncMutex/AsyncSemaphore의
        // YieldingPolicy가 내부적으로 이 패턴을 쓴다)의 **재개**
        // 지점이기도 하다. 최초 진입 때와 달리 재개 시점엔 이
        // AsyncTask 전용 스택이 `AsyncTask::yield()`의
        // `kContextSwitch` 반환 지점(onExec 본문 한가운데, 예: 위
        // while 루프 다음 줄의 `args->bridge = ...`처럼 제출자의 유저
        // 포인터를 그대로 역참조하는 코드)으로 곧장 떨어지는데, 그
        // 사이 리액터가 다른 Task/AsyncTask를 실행하며 CR3를 얼마든지
        // 바꿔 놨을 수 있어 이 지점의 CR3가 이 AsyncTask 제출자의
        // 주소공간이라는 보장이 전혀 없었다 - 실측으로 Page Fault(때로
        // GPF/Invalid Opcode, cr3가 다른 Task의 것이거나
        // gBootPml4Phys) 재현(pn584db994_connector/accepter TEMP
        // 유저랜드 스트레스 하네스, GRUB SMP4, 40회 중 8회 재현).
        // 최초 진입 시에는 `kAsyncTaskEntryWrapper` 자신의 내부
        // 동기화가 이미 같은 값으로 한 번 더 맞추므로(두 번째 호출은
        // "이미 맞음"을 확인만 하고 실제 `mov cr3`는 스킵) 무해한
        // 중복이다.
        const uint64_t savedPml4ForStackpoolResume = kSyncCr3ForAsyncExecEntry(task);
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
            // [신규, 2026-09-23, PN-E4C6AF72] 이 kContextSwitch가 물리적으로
            // 인터럽트 디스패치 스택을 떠나는 정확한 지점 - kAsyncDrainIsr
            // (인터럽트 컨텍스트)에서 호출된 경우, 이 구간 동안 또 다른
            // 일반 인터럽트가 이 코어에 들어오면 gSavedTaskRsp[coreIndex]
            // 단일 슬롯이 덮어써질 수 있다는 게 PN-3DDF2797이 확정한 근본
            // 원인이다 - 그 가설을 gdb 없이 검증하기 위한 비관측적 기록.
            // [확장, 2026-09-24, PN-E4C6AF72 §"남은 것" 0번] CS 진단을
            // Enter/LeaveInterruptStack/DynDispatchEnter/Exit에 이어 이
            // 지점에도 남긴다 - kContextSwitch 전후로 CS가 이미 오염돼
            // 있었는지(그 이전 구간의 문제) 아니면 이 kContextSwitch
            // 자체가 오염을 유발하는지를 구분하기 위한 것.
            kDiagRingLog(DiagRingEvent::StackfulDispatchBegin, coreIndex, 0, 0, kReadCurrentCodeSegment());
            kContextSwitch(&gReactorSavedRsp[coreIndex], task->tcb);
            kDiagRingLog(DiagRingEvent::StackfulDispatchEnd, coreIndex, 0, 0, kReadCurrentCodeSegment());
        }
        // [PN-584DB994] coroHandle 분기와 동일 - 리액터/idle 컨텍스트로
        // 돌아가기 전 CR3를 이 재개/진입 이전 값으로 되돌린다.
        kRestoreCr3AfterAsyncExecEntry(savedPml4ForStackpoolResume);
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
        kWakeWaitingAsyncTask(task);
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

// [신규, 2026-09-22, PN-A0CEF82D/QU-CC8A31F6] async_task.h의
// AsyncTaskCoroYield 문서 주석 참고 - AsyncReactor가 이 시점(async_task.h
// 안의 선언 위치)엔 아직 전방 선언조차 없어 여기(AsyncReactor가 이미
// 완전한 타입인 지점)에 정의한다. self가 null이면(코루틴 onExec
// 밖에서 잘못 호출된 경우 - 계약 위반) 정지는 되지만 아무도 다시
// 깨우지 않아 사실상 멈춘다 - 방어 이상의 조치는 하지 않는다(호출부
// 책임, AsyncTaskCoroAwaiter::await_suspend()의 동일한 null 처리와
// 같은 관례).
void AsyncTaskCoroYield::await_suspend(std::coroutine_handle<>) noexcept {
    AsyncTask* self = AsyncTask::current();
    if (self) {
        // [신규, 2026-09-26, PN-F2594E93/DC-5F0AC0D3, 설계자 답변 "(b)"]
        // async_task.h의 kAsyncTaskCoroYieldFairnessThreshold/
        // coroYieldRetryStreak 문서 주석 참고 - 연속 재시도가 문턱을
        // 넘으면 이번만 preemptive=false로 강등해 일반 큐도 반드시
        // 차례를 받게 한다(우선순위 역전 라이브락 방지). 강등한
        // 그 순간 카운터를 0으로 되돌려, 다음 문턱까지 다시 "자기
        // 완결적 폴링" 원래 의도(즉시 재확인)를 그대로 유지한다.
        ++self->coroYieldRetryStreak;
        bool demote = self->coroYieldRetryStreak >= kAsyncTaskCoroYieldFairnessThreshold;
        if (demote) {
            self->coroYieldRetryStreak = 0;
        }
        AsyncReactor::submitCompletion(self, /*preemptive=*/!demote);
    }
}

}  // namespace kernel
