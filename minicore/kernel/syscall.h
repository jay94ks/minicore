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

// [신규, 2026-09-18, SP-76250478 §3 항목2, PN-0EB2FABF] `SelfTerminate`
// (위)의 스레드 전용 대칭(RM-48E1E610 그룹0 #9) - `SelfTerminate`는
// "이 UserThread가 끝나는 순간 Process 전체가 끝난다"(POSIX `exit()`와
// 동일 의미, 미처리 예외/신호/자연 종료가 전부 이 경로)는 뜻이지만,
// `SelfTerminateThread`는 "이 스레드 하나만 끝난다"(POSIX `pthread_exit()`
// 와 동일 의미)는 뜻이다 - `CreateThread`가 만든 스레드가 정상 종료할
// 때(유저랜드 C 런타임의 스레드 진입 트램폴린이 `entry`의 반환값을
// 잡아 이 syscall을 대신 호출) 쓴다. **`SelfTerminate`와 마찬가지로
// 이 syscall도 절대 ring3로 복귀하지 않는다**(아래 `kThreadOnFallingToEnd`
// 참고) - `idt.cpp`의 `kDispatchSyscallVerbBody`가 이 endpointId도
// `kSyscallEndpointSelfTerminate`와 동일하게 특별 취급한다. 다만 실제
// 핸들러(`SelfTerminateThreadHandler`, scheduler.cpp)는 `process->
// destroy()`를 무조건 부르지 않는다 - `Process::threads`(process.h)에서
// 이 스레드만 좀비 표시/회수하고, 그 결과 `threads`가 실제로 완전히
// 비었을 때만(이론상 모든 스레드가 main 포함 이 syscall로 끝나야만
// 도달) `SelfTerminateHandler`와 같은 프로세스 종료 마무리 로직
// (`kFinalizeProcessTermination`, scheduler.cpp에 공용으로 뺌)을
// 부른다.
constexpr SyscallEndpointId kSyscallEndpointSelfTerminateThread = kMakeSyscallEndpointId(0, 9);

// SelfTerminateThread 인자 - 유일한 입력은 exitCode 하나뿐이고(POSIX
// `pthread_exit(void*)`의 단순화판, 포인터 대신 정수 하나), out
// 파라미터가 없다(호출부로 절대 안 돌아오므로 의미가 없음).
struct SelfTerminateThreadArgs {
    int32_t exitCode = 0;
};

// int 0x80/`syscall` 명령 두 트랩 경로가 공유하는 공용 verb 디스패치
// (PN-124C105B, QU-E7E51931/QU-CD6F68B7로 확정된 ABI 그대로) - RAX=verb
// (0=submit/1=wait), RDI/RSI=verb별 인자, 반환값이 새 RAX가 된다.
// **self-terminate(verb=submit + endpointId=kSyscallEndpointSelfTerminate)
// 는 이 함수가 반환하지 않는다** - `kTaskOnFallingToEnd()` 호출 후
// `frame`이 있으면(=int 0x80) `Scheduler::parkFromISR()`로, 없으면
// (=syscall 빠른 경로) sti+hlt 루프로 영원히 대체되므로, 양쪽 트랩
// 스텁(idt.cpp의 int 0x80 경로, syscall_fastpath.cpp의 `syscall`
// 경로) 모두 "이 함수가 반환하지 않으면 그 뒤 ring3 복귀 코드
// (iretq/sysretq)도 실행되지 않는다"는 계약에 이미 의존하고 있다 -
// 정의는 idt.cpp(기존 int 0x80 핸들러가 있던 자리, kTaskOnFallingToEnd/
// Syscall::submit·wait 전부 이미 그쪽에서 쓰고 있었음).
//
// [수정, 2026-09-21, PN-1DFCB337] `frame` 인자 신설 - int 0x80
// 경로(idt.cpp의 kHandleSyscallTrap)는 isr_common_stub이 이미
// gInterruptDepth를 늘려 둔 진짜 InterruptFrame을 그대로 넘기고,
// `syscall` 빠른 경로(syscall_fastpath.cpp)는 애초에 그 카운터를
// 안 건드리므로 null을 넘긴다 - self-terminate류 분기가 이 값의
// 유무로 "카운터를 닫아야 하는지"를 판단한다(kCheckSignalCheckpoint
// 문서 주석 참고, idt.cpp).
uint64_t kDispatchSyscallVerb(uint64_t verb, uint64_t arg0, uint64_t arg1, InterruptFrame* frame);

// [신규, 2026-09-18, SP-76250478 §2.1, PN-0EB2FABF] 프로세스 안에서만
// 유일한(전역 유일 아님) 스레드 식별자 - `Process::threads`가 단일
// `mainThread` 포인터를 대체하며 함께 도입됐다(process.h 참고). 16비트로
// 좁힌 이유는 설계자 opinion 그대로("16비트 정수로, 프로세스 내에서만
// 유일") - 프로세스 하나가 65535개 넘는 스레드를 가질 일은 없다(아래
// kMaxThreadsPerProcess, process.h 참고).
using ThreadId = uint16_t;
constexpr ThreadId kInvalidThreadId = 0xFFFFu;

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
// [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] `Process::mainThread`
// (raw UserThread*) 단일 필드가 `Process::threads`(ChunkedList<
// SharedPtr<UserThread>, 16>)로 바뀌었지만, 그 컨테이너에 담기는
// `SharedPtr<UserThread>`는 이 클래스의 기존 `_selfRef`와 정확히 같은
// no-op 삭제자 인스턴스를 그대로 복사해 넣은 것뿐이다 - **실제 슬랩
// 메모리 반납은 여전히 `UserThread::release()`가 명시적으로 담당**
// 한다(아래 `_selfRef` 주석 참고). `threads`는 "누가 이 스레드를
// 프로세스의 스레드 목록에 포함시켰는가"라는 소속 정보의 컨테이너일
// 뿐, SharedPtr의 참조 카운트가 0이 된다고 슬랩 메모리가 자동으로
// 반납되지는 않는다 - 호출부는 반드시 `threads`에서 슬롯을 지운
// **뒤에** `UserThread::release()`를 불러야 한다(반대 순서면 다른
// 관찰자가 그 사이 컨테이너를 순회하다 이미 반납된 메모리를 살아있는
// 스레드로 오인할 수 있다).
// [승격, 2026-09-19, SP-9A6D579F §3.5, PN-06A7C439] 원래 debug_session.h
// 전용이었으나 여기로 옮겼다(async_task.h의 `AsyncTaskWeakRef` 승격과
// 동일한 순환-include 회피 패턴 - debug_session.h가 이미 이 헤더를
// include하므로 그쪽은 그대로 이 정의를 재사용한다). `InterruptFrame`
// 자체를 syscall ABI로 그대로 노출하지 않는 이유(`vector`/`errorCode`
// 제외) 등 상세 문서는 debug_session.h 상단 주석 참고 - 필드 목록만
// 여기로 이동. **[재정리, 2026-09-19, QU-47A83CDF 답변("혼재된 것들을
// 리팩토링해야 할 것 같네")]** 이 타입은 이제 순수하게 syscall ABI의
// 유저-커널 경계 값(디버거가 넘기는/받는 버퍼 모양)일 뿐이다 -
// `UserThread`는 더 이상 이 타입으로 된 자기 소유 사본을 갖지 않는다
// (아래 `debugLiveFramePtr`가 유일한 진짜 상태 - "잡아 둔 진짜
// InterruptFrame"과 "그 값의 별도 복사본"이라는 두 갈래로 쪼개져 있던
// 것을 하나로 합쳤다, debug_session.cpp의 `kCopyFrameToSnapshot()`/
// `kCopySnapshotToFrame()`이 이 경계에서만 필요한 변환을 담당).
struct DebugRegisterSnapshot {
    uint64_t rax = 0, rbx = 0, rcx = 0, rdx = 0, rsi = 0, rdi = 0, rbp = 0;
    uint64_t r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;
    uint64_t rip = 0;
    uint64_t cs = 0;
    uint64_t rflags = 0;
    uint64_t rsp = 0;
    uint64_t ss = 0;
};

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

    // [신규, 2026-09-18, SP-76250478, PN-0EB2FABF] `Process::threads`
    // (process.h)에 이 스레드 자신을 등록하려는 외부 호출부(process.cpp의
    // execImage()/fork() - UserThread의 멤버 함수가 아니다)는
    // `sharedFromThis()`(protected)에 접근할 수 없어 `weakAsTask()`와
    // 동일한 이유로 이 공개 래퍼가 필요하다. 반환된 SharedPtr은 이
    // 스레드의 `_selfRef`와 컨트롤 블록을 공유하는 별개의 강한 참조
    // 인스턴스일 뿐 - 클래스 문서 주석대로 실제 슬랩 반납은 여전히
    // `UserThread::release()`가 명시적으로 담당한다.
    SharedPtr<UserThread> sharedSelf() { return sharedFromThis(); }

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

    // [신규, 2026-09-18, SP-76250478 §2.1/§3/§3.1, PN-0EB2FABF] 멀티스레드
    // 유저 프로세스 지원 - `Process::threads`(process.h)에 담기면서
    // 함께 도입된 필드들.
    // [갱신, 2026-09-18, PN-0EB2FABF 2단계] `threadId`는 이제 실제로
    // 배선됐다 - `execImage()`/fork()의 최초 스레드와 `CreateThread`
    // (process.cpp)가 만드는 스레드 전부 `Process::nextThreadId`에서
    // 발급받는다. `isZombie`/`exitCode`/`detached`/`joinerAsyncTask`는
    // 여전히 미배선 - `SelfTerminateThread`/`Join`/`Detach`(후속
    // 증분)가 실제로 소비한다.
    ThreadId threadId = kInvalidThreadId;  // CreateThread/execImage()/fork()가 발급(§2.1)
    // [갱신, 2026-09-18, PN-0EB2FABF 3단계] `isZombie`/`exitCode`는 이제
    // 실제로 배선됐다 - `SelfTerminateThreadHandler`(scheduler.cpp)가
    // 정상 종료 시(§3 항목2) 기록한다. `Process::isZombie`(프로세스
    // 트리 좀비, §6)와는 별개 축이다.
    bool isZombie = false;   // 정상 종료 후 아직 Join되지 않은 상태
    int32_t exitCode = 0;    // SelfTerminateThread가 기록(§3 항목2)
    // [갱신, 2026-09-18, PN-0EB2FABF 3단계] `detached`도 실제로 배선됐다 -
    // 세팅돼 있으면 `SelfTerminateThreadHandler`가 좀비 단계를 건너뛰고
    // 그 자리에서 즉시 `threads`에서 지우고 슬랩까지 반납한다. 아직
    // 이 값을 세팅하는 `Detach` syscall(§3 항목3, 후속 증분)이 없어
    // 지금은 항상 false로 남는다.
    bool detached = false;
    // [정정, 2026-09-18, PN-0EB2FABF 4단계 착수 중 발견] §3.1 - 이
    // 스레드가 좀비가 되는 순간(SelfTerminateThreadHandler가) 직접
    // 재개시켜야 할 Join() 대기자의 AsyncTask(있다면 단 하나, v1은
    // 다중 joiner 미지원)를 안전하게 참조하는 값 - **Phase 1이 적어
    // 둔 `WeakPtr<AsyncTask>` 스케치는 실제로 쓸 수 없었다**: `WeakPtr<T>`
    // 는 `T`가 `kMakeShared`로 만들어진(또는 `EnableSharedFromThis<T>`
    // 를 상속한) 대상이어야 컨트롤 블록을 가리킬 수 있는데, `AsyncTask`
    // 는 항상 raw slab 메모리 위에 놓이는 원시 구조체라(SP-F682B889
    // §3.1 "처리기가 생성/해제 전부 책임") 그런 컨트롤 블록 자체가
    // 없다. 이 커널이 "AsyncTask를 그 수명과 독립적으로 안전하게
    // 관찰"해야 하는 문제를 이미 겪어 풀어 둔 게 `AsyncTaskWeakRef`
    // (async_task.h, 원래 `AsyncTask::scheduleTimeout()` 전용이었으나
    // 이번 증분에서 공개 재사용 primitive로 승격, 그 클래스 문서 참고)
    // - Join도 그 정확히 같은 문제라 그대로 재사용한다. `AsyncTask::
    // ensureWeakRef()`로 얻고 `addRef()`로 이 필드 몫을 등록, 다 쓰면
    // (SelfTerminateThreadHandler가 소비한 뒤) `release()`로 그 몫을
    // 내려놓는다 - process.cpp JoinHandler/scheduler.cpp
    // SelfTerminateThreadHandler 양쪽 참고.
    AsyncTaskWeakRef* joinerAsyncTask = nullptr;

    // [신규, 2026-09-18, SP-76250478 §2.2, PN-0EB2FABF] `CreateThread`
    // (process.cpp)가 만든 스레드에서만 쓴다 - `kEnterRing3Thread`
    // (process.cpp)가 ring3 진입 직전 이 값을 RDI에 실어 `entry(arg)`
    // SysV 관례를 만족시킨다. `execImage()`/fork()가 만드는 스레드는
    // (ELF `_start`/재개 프레임을 각자 다른 방식으로 쓰므로) 이 필드를
    // 전혀 안 씀(0으로 남음).
    uint64_t threadStartArg = 0;

    // [신규, 2026-09-18, SP-76250478 §3 항목2, PN-0EB2FABF] `CreateThread`
    // 가 `ProcessAddressSpaceManager::mapRegion()`으로 확보해 준 이
    // 스레드 전용 스택의 [시작, 길이) - `SelfTerminateThreadHandler`가
    // 이 스레드가 끝나는 즉시(좀비 단계와 무관하게 - 어차피 다시는
    // 실행되지 않으므로) `unmapRegion(threadStackBase, threadStackSize)`
    // 로 그 VMA를 회수하는 데 쓴다. `ProcessAddressSpaceManager`가
    // 최대 8개 VMA만 지원하는 희소 자원이라(address_space.h 클래스
    // 문서), 아직 Join되지 않은 좀비 스레드라도 스택만은 즉시
    // 돌려받아야 한다 - `UserThread` 구조체 자신(threadId/exitCode
    // 보관용)은 Join()이 회수할 때까지 남지만 스택은 그럴 필요가 없다.
    // `execImage()`/fork()가 만드는 스레드는 고정 스택(`registerFixedRegion`)
    // 이라 이 필드를 전혀 안 씀(0으로 남음 - `destroy()`의 `unmapAll()`
    // 이 대신 회수).
    uint64_t threadStackBase = 0;
    uint64_t threadStackSize = 0;

    // [신규, 2026-09-19, SP-9A6D579F §1-A/§3.5, PN-06A7C439] 이 스레드가
    // 디버거에 의해 정지된 순간의 레지스터 상태 - 원래
    // `Process::debugSession`에 프로세스당 하나만 있었으나, `pausedByDebugger`
    // (여전히 process-wide 플래그, debug_session.h 참고)로 인해 같은
    // 프로세스의 여러 스레드가 서로 다른 코어에서 각자 다른 순간에
    // 동시에 정지 상태로 들어갈 수 있게 된 이상 "정지된 그 순간의 값"
    // 자체는 스레드마다 독립적이어야 한다(process당 하나면 두 번째로
    // 정지하는 스레드가 첫 번째 스레드의 스냅숏/살아있는 프레임을
    // 덮어써 버리는 레이스가 생김). `kSaveDebugRegistersSnapshot()`
    // (debug_session.cpp)이 이 스레드가 실제로 Blocked로 전환되는 그
    // 순간(onTick() 재스케줄 결정 지점, 또는 #DB ISR이 즉시 파킹하는
    // 경로)에 채운다.
    // nullptr이 아니면 "이 스레드가 지금 유효한 정지 상태(디버거가
    // 관찰/수정 가능)"라는 뜻 - `DebugGetRegisters`/`DebugSetRegisters`
    // (targetThread로 이 스레드를 골랐을 때)와 `DebugContinue`(정지된
    // 스레드 전부를 순회할 때)가 이 조건으로 "이 스레드가 지금 대상이
    // 될 수 있는지"를 판단한다. **[재정리, 2026-09-19, QU-47A83CDF
    // 답변]** 예전엔 이 포인터가 가리키는 살아있는 진짜 `InterruptFrame`
    // 과 별도로 `debugSavedRegisters`(값 사본)가 있어, GetRegisters/
    // SetRegisters는 그 사본만 건드리고 DebugContinue가 재개 직전
    // 사본→진짜 프레임으로 write-back하는 3단계(캡처/수정/반영) 구조
    // 였다 - "이 스레드의 정지된 레지스터 상태"라는 개념 하나가 두
    // 곳에 나뉘어 있어 어느 한쪽만 보면 정확한 판단이 안 되는 문제가
    // 있었다. 이제 그런 별도 사본이 없다 - `DebugGetRegisters`/
    // `DebugSetRegisters`가 `*debugLiveFramePtr`를 직접 읽고 쓴다(경계
    // 변환은 `kCopyFrameToSnapshot()`/`kCopySnapshotToFrame()`,
    // debug_session.cpp). 이 포인터가 가리키는 메모리는 이 스레드
    // 자신의 커널 스택 위, 아직 그 자리에 그대로 있다(다른 무엇도 그
    // 스택을 건드리지 않는다는 게 이 매커니즘의 전제, debug_session.h
    // 상단 주석 참고) - `DebugContinue`가 재개 직전 RFLAGS.TF/RF만
    // 필요한 만큼 이 자리에서 직접 보정한 뒤 즉시 `nullptr`로 되돌린다
    // (재사용/댕글링 방지, `kWriteBackDebugFrame()` 참고).
    InterruptFrame* debugLiveFramePtr = nullptr;
    // 이 스레드에 대해서만 적용되는 다음 DebugContinue의 싱글스텝 요청 -
    // "한 번 쓰이면 소비되는" 값(debug_session.h의 옛 `DebugSession::
    // singleStepPending` 문서 주석과 동일한 의미, 스레드별로 독립됨).
    bool debugSingleStepPending = false;

    // [SP-6BEAE0C1 §5, PN-543C0CE9] 동적 UserThread 풀 - Process::
    // allocate()/release()와 완전히 같은 이유/같은 안전 전제(모든
    // 필드가 0/nullptr NSDMI라 memset 결과가 실제 생성자 결과와 동일,
    // 정의는 syscall.cpp 참고). 반환값은 아직 Task::init()을 부르지
    // 않은 "빈 자리".
    //
    // [수정, 2026-09-17, PN-B4987BF6] **외부에서 관찰되는 계약은 전혀
    // 안 바뀐다** - `allocate()`가 여전히 raw 포인터를 돌려주고,
    // [수정, 2026-09-18, SP-76250478/PN-0EB2FABF] `Process::threads`가
    // 여전히 그 유일한 진짜 소유자이며(위 클래스 문서 주석 참고 -
    // `mainThread` 단일 필드에서 컨테이너로 바뀌었을 뿐 "명시적 반납"
    // 계약 자체는 그대로), `release()`가 여전히 그 소유자가 다 쓴
    // 시점에 명시적으로 반납을 결정한다. 내부적으로만 `_selfRef`(아래)를
    // 통해 `kMakeShared`의
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
    // 놓는다 - "실제 소유자는 여전히 Process::threads가 붙든 명시적
    // 반납 계약"(2026-09-18, SP-76250478/PN-0EB2FABF 갱신 - 옛
    // `mainThread` raw pointer에서 컨테이너로 바뀌었을 뿐 계약은 동일)
    // 이라는 기존 계약을 그대로 유지하면서, `EnableSharedFromThis`가
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
