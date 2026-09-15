#ifndef MINICORE_KERNEL_TASK_H
#define MINICORE_KERNEL_TASK_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"

namespace kernel {

class Waitable;  // 포인터로만 참조(Task::blockedOn) - 전체 정의는 waitable.h(SP-0666DB3C §9.2)

// SP-0666DB3C §11 - 명시적(explicit) TLS 슬롯 개수. Task::tlsSlots의
// 배열 크기이자 TlsRegistry::allocateSlot()(tls.h)의 발급 상한이라
// 두 파일이 서로를 포함하지 않도록 이 값 자체를 Task와 같은 헤더에
// 둔다(ThreadLocal<T>/TlsRegistry 전체 선언은 tls.h 참고).
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
};

// affinityMask 비트 i가 1이면 코어 i에서 실행 가능 - "이 코어에서만"
// (허용 목록)과 "이 코어에서는 불가"(제외 목록) 둘 다 전체 마스크에서
// 시작해 비트를 세우거나/지우는 것으로 자연스럽게 표현된다.
constexpr uint32_t kTaskAffinityAllCores = 0xFFFFFFFFU;
constexpr uint64_t kTaskDefaultKernelStackSize = 8UL * 1024UL;  // 8KiB(QU-BA001D73)

using TaskEntry = void (*)(void* arg);

// context_switch.S의 kContextSwitch/kTaskStartTrampoline이 이 구조체의
// savedRsp 오프셋(항상 첫 필드, 오프셋 0)을 그대로 참조한다 - 필드
// 순서를 바꾸려면 그쪽 어셈블리도 같이 확인해야 한다.
struct Task {
    // 이 Task가 스위칭 아웃될 때의 RSP(커널 스택 안, kContextSwitch가
    // push한 레지스터들의 맨 위를 가리킨다). init() 직후엔 아직 한
    // 번도 실행되지 않은 상태로 kTaskStartTrampoline에 진입하도록
    // 미리 꾸며 둔 스택을 가리킨다.
    uint64_t savedRsp = 0;

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

    // 이 Task가 ring3 유저 코드로 격하(demote)된 적이 있으면 true -
    // 아직 이 프로젝트엔 그 격하 메커니즘 자체가 없어(프로세스 모델
    // 미착수) 항상 기본값 false로 남는다. kTaskFallingToEnd(entry가
    // 반환해 이 Task의 실행이 자연 종료되는 지점, context_switch.S)가
    // 이 플래그로 종료 처리를 분기한다(PL-2D3184BC "Task 종료
    // 프로토콜", QU-26F9420E 설계자 답변, 2026-09-14) - true면
    // 자기종료 syscall만 제출, false(지금 항상 이 경우)면
    // Scheduler::retireCurrentTask()로 스케줄러에서 완전히 떼어낸다.
    bool isUserLevel = false;

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

    // 이 Task가 지금 무엇에 막혀 파킹돼 있는지(SP-0666DB3C §9.2, 임의
    // 대기 상태를 강제로 끄집어내는 범용 훅) - 파킹 시작 시 그 대기
    // 구조체 자신(WaitQueue 등)이 설정하고, 깨울 때(정상 wakeOne()이든
    // 강제 cancel()이든) 같은 대기 구조체가 자신의 락 아래에서 다시
    // nullptr로 되돌린다(§9.6-1 - 정상 웨이크업과 강제 취소 두 경로가
    // 경쟁해도 정확히 한쪽만 성공하도록, 이 필드 정리 자체를 그 락으로
    // 직렬화한다). 대기 중이 아니거나 실행 중이면 nullptr.
    Waitable* blockedOn = nullptr;

    // WaitQueue(SP-0666DB3C §1/§5-1)가 이 Task를 파킹시킨 코어 - 나중에
    // wakeOne()/cancel()이 Scheduler::scheduleImmediate(parkedCoreIndex,
    // this)로 정확히 그 코어에서 재개시키는 데 쓴다(다른 코어로 옮겨
    // 깨우는 로드밸런싱은 v1 범위 밖, §5-1).
    uint32_t parkedCoreIndex = 0;

    // 명시적(explicit) Thread Local Storage 슬롯 배열(SP-0666DB3C §11) -
    // 진짜 컴파일러 thread_local이 아니라 TlsRegistry::allocateSlot()로
    // 발급받은 인덱스를 ThreadLocal<T>가 그대로 이 배열에 꽂아 쓴다.
    // 각 슬롯이 가리키는 실제 인스턴스의 생성/해제는 그 슬롯을 발급받은
    // 서브시스템 책임 - Task 자신은 포인터 배열만 소유한다.
    void* tlsSlots[kMaxTlsSlots] = {};

    // 커널 스택을 새로 할당하고, entry(arg)를 처음 실행할 준비가 된
    // 상태로 초기화한다(트램폴린 스택 프레임 구성) - 스케줄러 큐에
    // 넣는 것은 호출부 책임(아직 스케줄러 자체가 없어 별도 API 없음).
    // Paging::init() 이후에만 호출 가능(direct map 필요).
    void init(TaskEntry entry, void* arg, uint64_t stackSize = kTaskDefaultKernelStackSize);
};

// 현재 실행 흐름의 레지스터 상태를 저장하고 newRsp로 전환한다 -
// *oldRspSlot에 전환 전 RSP를 기록한다. 소프트웨어 방식 컨텍스트
// 스위칭(DC-8EA1E7F6/QU-BA001D73 - "하드웨어 TSS 대신 소프트웨어
// 스위칭") - x86_64 System V 콜리세이브 레지스터(rbx/rbp/r12-r15)와
// RFLAGS만 저장/복원한다(caller-saved 레지스터는 C++ 호출 규약상
// 이미 호출부가 필요하면 자기 스택에 저장해 뒀을 것이므로 안 건드림).
extern "C" void kContextSwitch(uint64_t* oldRspSlot, uint64_t newRsp);

}  // namespace kernel

#endif  // MINICORE_KERNEL_TASK_H
