#ifndef MINICORE_KERNEL_SYSCALL_H
#define MINICORE_KERNEL_SYSCALL_H

#include "async_task.h"
#include "interrupt_frame.h"
#include "libkenv/chunked_list.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/types.h"
#include "task.h"

namespace kernel {

class Process;  // 포인터로만 참조(UserThread::process) - 전체 정의는 process.h

// 공개 ABI로 노출되는 syscall 번호 - 커널이 부팅 시 고정 배정한다
// (동적 재배정 없음, SP-04EE2A18). AsyncCallbackRegistry가 내부적으로
// 동적 배정하는 subjectCode와는 별개의 이름 공간이다(아래
// SyscallRegistry 참고).
using SyscallEndpointId = uint32_t;

// [신규, 2026-09-17, SP-E9B44929, 설계자 지시("Syscall Group -> Syscall
// 로 맵핑되는 방식으로... 하위 8비트는 Syscall 번호, 그외 상위 비트는
// 그룹 번호")] `SyscallEndpointId`의 와이어 타입/트랩 ABI는 전혀 안
// 바뀐다(여전히 uint32_t, RDI로 전달) - 그 32비트 값의 **해석**만
// 그룹(비트 15:8)+그룹 내 call 번호(비트 7:0)로 나뉜다. 비트 31:16은
// 예약(반드시 0) - `SyscallRegistry`가 그 외 값을 즉시 거부한다.
// 각 그룹의 call 번호는 그 그룹을 소유한 subsystem 문서가 다른
// 그룹과 조율할 필요 없이 0부터 독자적으로 채운다(RM-48E1E610의
// "그룹 배정" 절이 그룹 번호만 중재, call 번호는 안 건드림).
constexpr uint32_t kSyscallCallBits = 8;
constexpr uint32_t kSyscallCallMask = 0xFF;
constexpr uint32_t kSyscallReservedMask = 0xFFFF0000u;  // 비트 31:16 - 반드시 0

constexpr SyscallEndpointId kMakeSyscallEndpointId(uint8_t group, uint8_t call) {
    return (static_cast<uint32_t>(group) << kSyscallCallBits) | call;
}
constexpr uint8_t kSyscallGroupOf(SyscallEndpointId id) {
    return static_cast<uint8_t>((id >> kSyscallCallBits) & 0xFF);
}
constexpr uint8_t kSyscallCallOf(SyscallEndpointId id) {
    return static_cast<uint8_t>(id & kSyscallCallMask);
}

// User-Level UserThread가 자연 종료(kTaskFallingToEnd)될 때 자기 자신을
// 종료 처리해 달라고 제출하는 예약 endpoint(PL-2D3184BC "Task 종료
// 프로토콜", QU-26F9420E 설계자 답변 1번, 2026-09-14 - "자기 자신을
// 종료처리하라는 System Call 명세를 별도로 만들고 그걸로 제출"). 핸들러
// (scheduler.cpp의 SelfTerminateHandler)가 Scheduler::init()에서 등록돼
// 있다(PN-71C3D483, QU-84E5B3D5 설계자 답변으로 확정) - 실행 흐름은
// kTaskOnFallingToEnd가 `Syscall::submitDetached()`로 제출하고,
// 핸들러의 onExec가 `Scheduler::retireTask()`로 실제 정리(커널 스택
// 회수)를 수행한다. **아직 남은 범위**: Process 자원 회수(주소공간
// unmap+Process::destroy())는 유저 영역에 매핑된 페이지 목록을 추적하는
// 자료구조(VMA/Maple Tree, SP-2AAD7C8D)가 아직 없어 이 핸들러가 하지
// 않는다 - 그 인프라가 생길 때 SelfTerminateHandler::onExec에 이어
// 붙인다. 또한 이 자연-종료 경로는 UserThread의 entry(항상
// process.cpp의 kEnterRing3, `[[noreturn]]`이라 정상적으로는 절대
// "반환"하지 않음)가 실제로 반환하는 경우에만 트리거되는데, 지금은
// 그런 경로가 없다 - ring3의 명시적 `mc::selfTerminate()` 호출(아직
// userland/libs/libmc/syscall.cpp의 `for(;;){}` 스텁)이 이 종료
// 시퀀스를 어떻게 트리거할지는 별도로 확인 필요(userland의 syscall
// 트랩이 절대 ring3로 돌아가면 안 된다는 점이 일반 syscall과 다름).
constexpr SyscallEndpointId kSyscallEndpointSelfTerminate = kMakeSyscallEndpointId(0, 0);

// int 0x80/`syscall` 명령 두 트랩 경로가 공유하는 공용 verb 디스패치
// (PN-124C105B, QU-E7E51931/QU-CD6F68B7로 확정된 ABI 그대로) - RAX=verb
// (0=submit/1=wait), RDI/RSI=verb별 인자, 반환값이 새 RAX가 된다.
// **self-terminate(verb=submit + endpointId=kSyscallEndpointSelfTerminate)
// 는 이 함수가 반환하지 않는다** - `kTaskOnFallingToEnd()` 호출 후
// sti+hlt 루프로 영원히 대체되므로, 양쪽 트랩 스텁(idt.cpp의 int 0x80
// 경로, syscall_fastpath.cpp의 `syscall` 경로) 모두 "이 함수가 반환하지
// 않으면 그 뒤 ring3 복귀 코드(iretq/sysretq)도 실행되지 않는다"는
// 계약에 이미 의존하고 있다 - 정의는 idt.cpp(기존 int 0x80 핸들러가
// 있던 자리, kTaskOnFallingToEnd/Syscall::submit·wait 전부 이미
// 그쪽에서 쓰고 있었음).
uint64_t kDispatchSyscallVerb(uint64_t verb, uint64_t arg0, uint64_t arg1);

// 유저 프로세스에 속한 스레드의 커널 쪽 표현(SP-04EE2A18, 설계자 지시
// 2026-09-14 - "커널 Task와 쓰레드는 다른 개념이다... 내부적으로 Task를
// 상속받아 유저 쓰레드를 구현해도 상관없다"). 이 이름 자체는 제안일
// 뿐 확정된 이름은 아니다 - 프로세스/스레드 모델을 실제로 설계할 때
// 최종 확정한다.
// [수정, 2026-09-17, PN-B4987BF6, DC-21647E46 로드맵 Phase 3
// QU-D8FE566E 답변("진행 - 안전성이 우선")] `EnableSharedFromThis`를
// 상속 - `AsyncTask::waitingTask`(WeakPtr<Task>)가 "이 UserThread가
// 아직 살아있는지"를 `.lock()`으로 확인할 수 있으려면 이 객체가 자기
// 컨트롤 블록을 가져야 한다. **강한 소유권 모델 자체는 안 바뀐다** -
// `Process::mainThread`(raw UserThread*)가 여전히 유일한 진짜
// 소유자이고 `UserThread::release()`가 여전히 그 시점에 실제로
// 반납한다(아래 `_selfRef` 주석 참고 - 외부에서 관찰되는 lifecycle은
// 100% 동일, `EnableSharedFromThis`는 순수하게 WeakPtr 관찰자 지원을
// 위한 부가 기능일 뿐이다).
class UserThread : public Task, public EnableSharedFromThis<UserThread> {
public:
    // [신규, 2026-09-17, PN-B4987BF6] `AsyncTask::waitingTask`를
    // 설정하는 호출부(syscall.cpp의 Syscall::waitForAnyOf, UserThread의
    // 멤버 함수가 아닌 외부 코드)는 `sharedFromThis()`(protected)에
    // 접근할 수 없어 이 공개 래퍼가 필요하다 - `Task::blockedOn`이
    // Mutex/Semaphore의 컨트롤 블록을 별칭(aliasing)하는 것과 정확히
    // 같은 패턴을, "컨테이너"가 곧 "관찰 대상 자신"인 경우에 적용한
    // 것뿐이다(별칭 대상이 `this`를 `Task*`로 업캐스트한 주소).
    WeakPtr<Task> weakAsTask() { return WeakPtr<Task>(sharedFromThis(), static_cast<Task*>(this)); }


    // 대기 중(아직 wait()/waitForMultipleSyscall()/
    // waitAnyForMultipleSyscall()로 소비되지 않은) syscall 하나 -
    // endpoint/token만 담는다("소유권" 자체는 이 값이 pendingSyscalls에
    // 들어있다는 사실 자체로 표현되므로 별도 valid 플래그 불필요,
    // ChunkedList::Slot::used가 그 역할을 대신한다).
    struct PendingSyscall {
        SyscallEndpointId endpoint = 0;
        AsyncTaskManageCode token = 0;
    };

    // 한 스레드가 동시에 여러 syscall을 제출/대기할 수 있어야 한다
    // (waitForMultipleSyscall/waitAnyForMultipleSyscall, SP-04EE2A18
    // QU-31402585/QU-F475C6C2 설계자 답변, 2026-09-14) - 그래서 단일
    // 필드가 아니라 청크 기반 연결 리스트(ChunkedList, libkenv, 재사용
    // 가능한 GENERIC 컨테이너로 만들라는 설계자 지시)에 여러 개를
    // 담는다(**단순 배열 금지** - 설계자가 명시).
    //
    // 청크 용량 10을 고른 이유: Slot{PendingSyscall(16B)+bool(1B, 8B로
    // 패딩)} = 24B, Chunk{Slot[10](240B)+next 포인터(8B)} = 248B -
    // GenericSlabAllocator의 7단계 버킷(SP-D7013B26) 중 256B 버킷에
    // 8B 낭비로 거의 꽉 채워 들어간다.
    static constexpr uint32_t kPendingSyscallChunkCapacity = 10;
    ChunkedList<PendingSyscall, kPendingSyscallChunkCapacity> pendingSyscalls;

    // 이 유저 스레드가 속한 프로세스(SP-8B6B8D25 §2-B, 유저 모드 페이지
    // 폴트를 그 프로세스의 PCB에 매다는 데 필요) - process.h가
    // UserThread를 참조하는 반대 방향 관계라 순환 include를 피하려고
    // 여기서는 전방 선언 타입으로만 갖는다(async_task.h의 `struct
    // Task;`와 동일한 관례 - `WeakPtr<Process>`는 `T*`/`ControlBlockBase*`
    // 두 포인터만 저장하므로 `Process`가 불완전 타입이어도 멤버로 둘
    // 수 있다, `T`가 완전해야 하는 연산은 이 필드를 실제로 쓰는
    // process.cpp/scheduler.cpp 등에서만 인스턴스화된다).
    //
    // [수정, 2026-09-17, PN-E2A114C1] `Process*`(관찰 포인터)에서
    // `WeakPtr<Process>`로 전환 - 이 스레드는 자신이 속한 프로세스의
    // 소유자가 아니다(진짜 소유자는 `Process::children`, DC-21647E46/
    // QU-76409699 "(B) 포함으로 읽자"). 사용부는 항상 `.lock()`으로
    // 유효성을 확인한 뒤 그 결과(`SharedPtr<Process>`)를 지역 변수로
    // 붙들고 쓴다 - 매번 다시 `.lock()`하면 그 사이 대상이 파괴될 수
    // 있다는 착시를 주지만, 실제로는 다시 lock한 결과도 여전히 같은
    // 대상을 가리킨다(대상이 살아있는 한) - 중요한 건 "이 스레드 자체가
    // 대상을 강제로 살려 두지 않는다"는 계약이지 매번 다른 결과가
    // 나온다는 뜻이 아니다.
    WeakPtr<Process> process;

    // [신규, 2026-09-18, PN-22E5E9E7 항목6, SP-29D652AA §5.2] 이
    // UserThread 전용 유저 thread_local 인스턴스의 FS_BASE 값 -
    // `Process::makeUserTlsInstance()`(process.cpp)가 그 프로세스의
    // PT_TLS 템플릿(항목5, `Process::hasTlsTemplate`)을 복사해 이
    // UserThread 소유 주소공간 안에 만든다. `Task::kernelFsBase`
    // (task.h)와 정확히 같은 x86_64 TLS variant II 관례(값 자체가
    // 템플릿 복사본 바로 뒤의 self-pointer 헤더 주소) - 다만 이건
    // 커널 슬랩이 아니라 **이 프로세스 자신의 유저 주소공간**(ring3
    // 코드가 %fs-상대로 직접 역참조하므로 PAGE_USER 매핑 필수)에 있다.
    // 프로세스에 템플릿이 없으면(`hasTlsTemplate=false`, v1 유저
    // 바이너리 전부 해당) 0으로 남는다. **아직 FS_BASE MSR에 실제로
    // 싣는 배선(syscall 진입/이탈, ring3 첫 진입)은 항목7 몫** - 이
    // 필드는 값을 마련해 두기만 한다.
    uint64_t userFsBase = 0;

    // [신규, PN-44C91D6E, fork() 자식 재개 경로] fork() syscall이 자식
    // UserThread를 만들 때 부모가 트랩한 시점의 전체 InterruptFrame을
    // 그대로 복사해(rax만 0으로 덮어씀) 여기 담아 둔다 -
    // `kResumeForkedRing3`(process.cpp)가 `Task::entry`로 처음 실행될
    // 때 이 값을 그대로 iretq해 "부모가 트랩한 바로 그 지점에서 재개"
    // 한다(execImage()의 `kEnterRing3`이 항상 고정 entryPoint+새
    // 스택을 가정하는 것과 정반대 경로). fork() 자식이 아닌 모든
    // UserThread는 이 필드를 전혀 안 씀(전부 0으로 남음).
    InterruptFrame forkResumeFrame{};

    // [SP-6BEAE0C1 §5, PN-543C0CE9] 동적 UserThread 풀 - Process::
    // allocate()/release()와 완전히 같은 이유/같은 안전 전제(모든
    // 필드가 0/nullptr NSDMI라 memset 결과가 실제 생성자 결과와 동일,
    // 정의는 syscall.cpp 참고). 반환값은 아직 Task::init()을 부르지
    // 않은 "빈 자리".
    //
    // [수정, 2026-09-17, PN-B4987BF6] **외부에서 관찰되는 계약은 전혀
    // 안 바뀐다** - `allocate()`가 여전히 raw 포인터를 돌려주고,
    // `Process::mainThread`가 여전히 그 유일한 진짜 소유자이며,
    // `release()`가 여전히 그 소유자가 다 쓴 시점에 명시적으로 반납을
    // 결정한다. 내부적으로만 `_selfRef`(아래)를 통해 `kMakeShared`의
    // 컨트롤 블록을 곁다리로 붙여 `weakAsTask()`/`WeakPtr<Task>`
    // 관찰자가 성립하게 한다.
    static UserThread* allocate();
    static void release(UserThread* thread);

    // [신규, PN-523B779F 조사 중 발견] `allocate()`를 거치지 않는
    // 정적 전역 UserThread(kmain.cpp의 `gInitThread`/`gServiceThread[]`
    // - init/devmgr/fs/net/tty의 최초 스레드)는 `_selfRef`가 영영
    // 채워지지 않아 `weakAsTask()`가 항상 빈 `WeakPtr<Task>`를
    // 반환한다 - `Syscall::submit()`이 그 빈 값을 그대로
    // `AsyncTask::submitterTask`에 넣으면, 나중에 `onExec()`이
    // `submitterTask.lock()`으로 제출자를 찾으려는 모든 시도(CR3
    // 동기화, `kValidateUserBuffer`류 포인터 검증 등)가 조용히
    // 실패한다 - devmgr의 `EnumerateDevices` 첫 호출이 유저 스택
    // 최상단 근처 Page Fault로 커널 PANIC까지 간 근본 원인이 바로
    // 이것(async_task.cpp의 CR3 동기화 자체는 정상 동작했으나, sync할
    // 대상 자체를 못 찾았다). `Process::execImage()`가 `thread->init()`
    // 직후 이 메서드를 호출해 두면(이미 `allocate()`로 채워져 있으면
    // 멱등하게 아무 일도 안 함) 정적/동적 UserThread 양쪽 다 이후로는
    // `submitterTask` 체이닝이 항상 성립한다.
    bool ensureSelfRef();

private:
    // [신규, 2026-09-17, PN-B4987BF6] `allocate()`가 `kMakeShared`로
    // 만든 강한 참조를 스스로 붙들고 있다가 `release()`가 명시적으로
    // 놓는다 - "실제 소유자는 여전히 Process::mainThread(raw pointer)"
    // 라는 기존 계약을 그대로 유지하면서, `EnableSharedFromThis`가
    // 필요로 하는 컨트롤 블록만 곁다리로 살려 두는 최소 장치다(no-op
    // 삭제자를 써서 `release()`가 `GenericSlabAllocator::free()`를
    // 직접 부르는 지금 방식과 정확히 같은 타이밍에 실제 반납이
    // 일어나게 한다 - `_selfRef.reset()`은 강한 참조 카운트만 0으로
    // 내릴 뿐, 그 자체가 메모리를 반납하지 않는다). 이 필드가 없으면
    // `EnableSharedFromThis::_weakThis`가 채워질 컨트롤 블록 자체가
    // 존재하지 않아 `weakAsTask()`가 항상 빈 WeakPtr을 반환한다.
    SharedPtr<UserThread> _selfRef;
};

// endpointId(공개 ABI, 고정 슬롯) <-> AsyncTaskHandler 매핑 - 내부적으로
// AsyncCallbackRegistry에 등록하고 그 결과 subjectCode(동적 배정)를
// endpointId 슬롯에 저장해 둔다. 이렇게 간접화하면 syscall 하나가
// AsyncTask 하나로 그대로 흐르는 기존 dispatch 경로
// (kAsyncTaskEntryWrapper -> AsyncCallbackRegistry::resolve)를 전혀
// 건드리지 않고 재사용할 수 있다 - endpointId 공간과 subjectCode
// 공간이 섞이지 않는다.
class SyscallRegistry {
public:
    // 이미 채워진 슬롯에 다시 등록하면 실패(설계 실수 조기 발견용).
    static bool registerHandler(SyscallEndpointId endpointId, AsyncTaskHandler* handler);

    // Syscall::submit이 실제 AsyncTask::submit에 넘길 내부 subjectCode를
    // 얻는 데 쓴다 - 등록 안 된 endpointId면 false.
    static bool resolveSubjectCode(SyscallEndpointId endpointId, AsyncTaskSubjectCode* outSubjectCode);
};

// syscall 하나 = AsyncTask 하나(SP-04EE2A18) - 제출(submit)은 즉시
// 반환, 실제 대기는 별도의 wait() 호출이 담당한다("제출/대기 분리"
// 확정 설계). **반드시 UserThread 실행 흐름에서만 호출해야 한다**
// (Scheduler::currentTask()를 UserThread*로 취급 - RTTI가 없어 호출부
// 책임으로 강제한다). 실제 ring3 syscall 트랩 진입점이 이 두 함수를
// 그대로 호출하게 될 예정이다(아직 트랩 진입 자체는 미구현).
class Syscall {
public:
    // endpointId가 등록돼 있지 않거나 AsyncTask/목록 슬롯 확보에
    // 실패하면 0(유효하지 않은 토큰)을 반환한다 - 블로킹하지 않는다.
    // 성공하면 호출한 UserThread의 pendingSyscalls에 {endpoint, token}
    // 항목 하나를 추가하고 그 토큰을 그대로 반환한다(한 스레드가 여러
    // 번 submit()해 여러 토큰을 동시에 들고 있을 수 있다).
    static AsyncTaskManageCode submit(SyscallEndpointId endpointId, void* args);

    // PN-71C3D483 - submit()과 달리 "제출하고 완전히 잊는다"(SP-F682B889
    // §3.1의 autoFree=true 패턴 그대로) - pendingSyscalls에 아무것도
    // 남기지 않고, 아무도 나중에 wait()으로 결과를 소비하지 않는다는
    // 전제다. 그래서 **UserThread 실행 흐름일 필요가 없다**(submit()과
    // 달리 `Scheduler::currentTask()`를 전혀 안 건드림) - 첫 소비자는
    // kTaskOnFallingToEnd의 self-terminate 제출(scheduler.cpp) - 종료
    // 중인 Task 자신은 그 결과를 절대 기다리지 않는다. endpointId가
    // 등록 안 돼 있거나 AsyncTask 확보에 실패하면 조용히 무시한다
    // (호출부가 결과를 확인할 방법 자체가 없으므로 반환값도 없다).
    static void submitDetached(SyscallEndpointId endpointId, void* args);

    // token이 호출한 UserThread 자신의 pendingSyscalls에 없으면(위조/
    // 타인 토큰, 또는 이미 소비된 토큰) 즉시 false. 이미 완료돼 있으면
    // 즉시 반환하고, 아직이면 완료될 때까지 블로킹한다(도중 풀려도
    // 유저랜드가 같은 token으로 다시 부르면 되므로 - 이 함수 자체가
    // 그 "다시 부름"과 완전히 동일한 코드 경로다, 별도 재합류 API
    // 불필요). 반환값은 AsyncTaskState::Completed로 끝났으면 true,
    // Failed로 끝났으면 false. 내부적으로 waitForAnyOf(토큰 1개짜리
    // 배열)와 완전히 같은 코드 경로를 탄다.
    static bool wait(AsyncTaskManageCode token);

    enum class MultiWaitOutcome { Completed, Failed, Invalid };

    struct MultiWaitResult {
        AsyncTaskManageCode token = 0;
        MultiWaitOutcome outcome = MultiWaitOutcome::Invalid;
    };

    // waitForMultipleSyscall/waitAnyForMultipleSyscall(SP-04EE2A18,
    // QU-31402585/QU-F475C6C2 설계자 답변, 2026-09-14) - 둘 다 넘겨준
    // tokens 중 이미 끝났거나(Completed/Failed) 유효하지 않은(자기
    // 소유가 아니거나 이미 소비된) 게 있으면 그중 하나를 즉시 반환하고,
    // 전부 아직이면 그중 아무 하나가 끝날 때까지 블로킹한다 - **둘의
    // 내부 메커니즘은 완전히 동일**하고(그래서 이 헤더에서도 같은
    // private 구현(waitForAnyOf)을 공유한다), 차이는 순수하게 "호출부가
    // 몇 번 부르는가"라는 사용 관례뿐이다:
    //
    // - waitForMultipleSyscall (AND 의미): tokens로 지정한 N개를 전부
    //   드레인하려면 호출부(유저랜드)가 "아직 결과를 못 받은 토큰들"만
    //   추려 이 함수를 최대 N번 반복 호출해야 한다.
    // - waitAnyForMultipleSyscall (OR 의미): tokens 중 아무 하나가
    //   끝나면 그걸로 답이 완성되므로 한 번만 불러도 충분하다.
    static MultiWaitResult waitForMultipleSyscall(const AsyncTaskManageCode* tokens, uint32_t count);
    static MultiWaitResult waitAnyForMultipleSyscall(const AsyncTaskManageCode* tokens, uint32_t count);

private:
    // wait()/waitForMultipleSyscall()/waitAnyForMultipleSyscall() 셋
    // 다가 공유하는 공용 구현 - "주어진 토큰 집합 중 하나가 끝나길
    // 기다린다"는 하나의 메커니즘.
    static MultiWaitResult waitForAnyOf(const AsyncTaskManageCode* tokens, uint32_t count);
};

// [신규, 2026-09-18, PN-10EE096A] `Syscall::waitAnyForMultipleSyscall()`을
// ring3 유저랜드 verb(2, idt.cpp의 `kSyscallVerbWaitAnyOf`)에 노출하는
// 인자 구조체 - `submit()`(RDI=endpointId, RSI=args)이나 `wait()`
// (RDI=token만)과 달리 입출력 필드가 여러 개라 구조체 포인터 하나로
// 묶는다(RDI=이 구조체를 가리키는 포인터). `waitForMultipleSyscall()`도
// 이 verb를 그대로 재사용한다 - 커널 내부에서 이미 완전히 같은 구현
// (`waitForAnyOf`)을 공유하고 "AND vs OR"의 차이는 유저랜드 호출부가
// 몇 번 부르는지에만 있다는 게 이미 확정된 설계라(QU-31402585/
// QU-F475C6C2/QU-C06793C2), verb를 2개로 나누지 않는다(RM-23F4B687
// §4 - 불필요한 중복 방지). `userland/libs/libmc/syscall.h`의
// `WaitAnyOfSyscallArgs`와 바이트 단위로 정확히 같은 레이아웃이어야
// 한다(channel.h/vfs.h와 동일한 수동 거울 복사 관례).
struct WaitAnyOfSyscallArgs {
    const AsyncTaskManageCode* tokens = nullptr;  // in
    uint32_t count = 0;                           // in
    // out
    AsyncTaskManageCode resultToken = 0;
    Syscall::MultiWaitOutcome resultOutcome = Syscall::MultiWaitOutcome::Invalid;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SYSCALL_H
