#ifndef MINICORE_KERNEL_DEBUG_SESSION_H
#define MINICORE_KERNEL_DEBUG_SESSION_H

#include "channel.h"
#include "interrupt_frame.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/types.h"
#include "syscall.h"

namespace kernel {

class Process;  // 포인터로만 참조(DebugSession::debuggerProcess) - 전체 정의는 process.h

// 프로세스 디버깅 서브시스템(SP-9A6D579F) - RM-48E1E610 50-58번 예약.
// 이번 증분은 §3.1(자료구조)과 §3.2/§3.3의 `DebugAttach`/`DebugDetach`
// (§8 착수 조건이 실제로 갖춰진 부분)만 다룬다 - 브레이크포인트/
// 싱글스텝/#DB ISR/메모리 읽기쓰기(§3.4-§3.6, 항목 3-6)는 하드웨어
// 디버그 레지스터·ISR을 직접 건드리는 별도 증분으로 미룬다.
// [갱신, 2026-09-17, SP-E9B44929] Debug 그룹(7).
constexpr SyscallEndpointId kSyscallEndpointDebugAttach = kMakeSyscallEndpointId(7, 0);
constexpr SyscallEndpointId kSyscallEndpointDebugDetach = kMakeSyscallEndpointId(7, 1);
constexpr SyscallEndpointId kSyscallEndpointDebugSetBreakpoint = kMakeSyscallEndpointId(7, 2);
// [구현 완료, 2026-09-17, SP-9A6D579F §3.4, PN-87D6B615 "남은 범위"
// 1번] DebugSetSingleStep(call 3) - RFLAGS.TF는 코어 레지스터가
// 아니라 그 Task 자신의 저장된 인터럽트 프레임/popfq 값 안에 있어
// kSyncDebugRegs류 "디스패치 시점에 다시 쓰기" 패턴을 못 쓴다고
// 봤으나, 실제로는 DC-47000304 (A)가 이미 만든 `savedRegisters`/
// `liveFramePtr` 인프라(§3.5, DebugGetRegisters/SetRegisters/
// DebugContinue)를 그대로 재사용하면 된다는 게 드러났다 - 이 syscall은
// 하드웨어를 전혀 건드리지 않고 `DebugSession::singleStepPending`
// 플래그만 세우고, 실제 RFLAGS.TF 반영은 `DebugContinue`가 write-back
// 하는 시점에 한다(아래 DebugSetSingleStepArgs/구현 주석 참고).
constexpr SyscallEndpointId kSyscallEndpointDebugSetSingleStep = kMakeSyscallEndpointId(7, 3);
//
// [신규, 2026-09-17, SP-9A6D579F §3.5/§4, PN-87D6B615, RM-48E1E610
// 7.4/7.7/7.8] `DebugContinue`(레지스터 접근 불필요, 그룹 freeze
// 교차 확인만 필요)와 `DebugReadMemory`/`DebugWriteMemory`(대상
// 주소공간을 Paging::translatePage()로 직접 순회하는 커널 대행 복사
// - 레지스터 프레임 위치와 무관).
constexpr SyscallEndpointId kSyscallEndpointDebugContinue = kMakeSyscallEndpointId(7, 4);
constexpr SyscallEndpointId kSyscallEndpointDebugReadMemory = kMakeSyscallEndpointId(7, 7);
constexpr SyscallEndpointId kSyscallEndpointDebugWriteMemory = kMakeSyscallEndpointId(7, 8);

// [신규, 2026-09-17, SP-9A6D579F §3.5, DC-47000304 (A) 채택 - 설계자
// 답변("인터럽트 그 자체가 다른 프로세스에게 실행 기회를 줘야 하는데
// 다른 방안을 사용하면 그렇게 할 수가 없어")] `DebugGetRegisters`/
// `DebugSetRegisters`(call 5/6) - 이 커널의 소프트웨어 컨텍스트
// 스위치(`kContextSwitch`)는 스택 자체를 스위칭하지 커널 스택 내용을
// 옮기지 않으므로, `Scheduler::onTick()`이 이 Task를 `Blocked`로
// 남기는 바로 그 순간 손에 쥔 `InterruptFrame*`(그 Task 자신의 커널
// 스택 위, isr_common_stub이 쌓아 둔 자리)가 이 Task가 다시
// 디스패치될 때까지 정확히 그 자리에 그대로 살아있다(다른 무엇도
// 그 스택을 건드리지 않음 - "인터럽트 자신이 다른 프로세스에게
// 실행 기회를 준다"는 게 바로 이 매커니즘: EOI 이후 코어는 다음
// Task로 넘어가고, 이 Task는 자기 스택에 그 프레임을 그대로 둔 채
// 그냥 대기한다). `kSaveDebugRegistersSnapshot()`이 그 살아있는
// 프레임 자신의 주소를 `UserThread::debugLiveFramePtr`(syscall.h)에
// 남겨 둔다(내부 전용, 어떤 syscall args에도 노출 안 됨). **[재정리,
// 2026-09-19, QU-47A83CDF 답변("혼재된 것들을 리팩토링해야 할 것
// 같네")]** 예전엔 이 값을 별도 사본(`DebugSession::savedRegisters`,
// 나중엔 `UserThread::debugSavedRegisters`)에 또 복사해 두고
// `DebugSetRegisters`는 그 사본만 바꾼 뒤 `DebugContinue`가 재개
// 직전 사본→진짜 프레임으로 write-back하는 3단계 구조였다 - "이
// 스레드가 정지 상태에서 갖는 레지스터 값"이라는 하나의 개념이
// 캡처본/진짜 프레임 둘로 쪼개져 있어 어느 쪽이 최신 진실인지
// 판단 지점이 늘어나는 문제가 있었다. 이제 별도 사본이 없다 -
// `DebugGetRegisters`/`DebugSetRegisters`가 `*debugLiveFramePtr`를
// (syscall ABI 경계에서만 `DebugRegisterSnapshot`으로 변환해) 직접
// 읽고 쓴다. `DebugContinue`는 여전히 재개 직전 필요하지만(RFLAGS.TF/
// RF 보정, 아래), 그건 "사본을 진짜 자리로 반영"이 아니라 "이미 유일한
// 진짜 자리 그 자체를 재개 가능한 상태로 마지막 손질"일 뿐이다.
constexpr SyscallEndpointId kSyscallEndpointDebugGetRegisters = kMakeSyscallEndpointId(7, 5);
constexpr SyscallEndpointId kSyscallEndpointDebugSetRegisters = kMakeSyscallEndpointId(7, 6);

// [SP-9A6D579F §3.1] DR0-DR3 하드웨어 슬롯 수와 동일 - 스레드마다
// 별도 슬롯이 아니라 프로세스당 공유. [갱신, 2026-09-19, PN-06A7C439]
// 멀티스레드 디버깅이 실제로 착수된 뒤에도 이 결론은 그대로다 - 하드웨어
// DR 레지스터 자체가 프로세스당 4개뿐이라 브레이크포인트는 영구히
// process-wide다(targetThread를 받지 않음). 대신 진짜 스레드별 상태
// (정지 순간의 레지스터 스냅숏/싱글스텝 요청)는 `UserThread`(syscall.h)
// 로 옮겼다 - 자세한 내용은 PN-06A7C439 본문 참고.
constexpr uint32_t kMaxDebugBreakpoints = 4;

// [신규, 2026-09-17, SP-9A6D579F §3.5, DC-47000304] `InterruptFrame`
// 자체를 syscall ABI로 그대로 노출하지 않는다 - `vector`/`errorCode`는
// ISR 자신의 장부일 뿐 "레지스터"가 아니다(디버거 입장에서 의미 없는
// 필드를 읽고 쓰게 하지 않기 위한 최소 API 위생, RM-23F4B687 §4). 그
// 외 필드는 `InterruptFrame`과 정확히 같은 이름/순서 -
// `kCopyFrameToSnapshot()`/`kCopySnapshotToFrame()`(debug_session.cpp)
// 이 이 경계에서만 필요한 변환을 담당한다. [승격, 2026-09-19,
// PN-06A7C439] `DebugRegisterSnapshot` 정의 자체는 `syscall.h`로
// 옮겼다 - 이 파일은 그 include를 통해 그대로 재사용한다. **[재정리,
// 2026-09-19, QU-47A83CDF 답변]** `UserThread`가 이 타입의 자기 소유
// 사본을 갖던 시절(`debugSavedRegisters`)은 지났다 - 순수하게 syscall
// 경계의 값 타입일 뿐이다(syscall.h `DebugRegisterSnapshot` 문서
// 주석 참고).

struct DebugBreakpoint {
    // [신규, 2026-09-17, SP-9A6D579F §3.4] DR7의 R/Wi 필드와 대응
    // (Read 단독 조건은 §7이 명시적으로 범위 밖으로 뺌).
    enum class Condition : uint8_t { Execute, Write, ReadWrite };
    bool enabled = false;
    uint64_t address = 0;
    Condition condition = Condition::Execute;
};

// [SP-9A6D579F §3.1, 구현 세부 확정] 원 설계 pseudocode는 이걸 전역
// 고정 배열(`gDebugSessions[kAcpiAllProcessesMax]`)로 그렸으나, 그
// 시점엔 아직 `Process`가 동적 슬랩 풀(PN-543C0CE9)이 아니었다 - SP
// 자신이 "실제 상한 상수명은 착수 시 기존 프로세스 테이블 상수
// 재사용"이라고 구현 세부로 열어 둔 자리를, 지금은 아예 **디버기
// `Process` 자신에 직접 매다는 방식**으로 채운다(RM-23F4B687 §4) -
// "프로세스당 세션 하나만"이라는 §7 영구 불변조건을 자료구조 자체가
// 강제하고, 전역 고정 배열/상한 자체가 필요 없어진다.
struct DebugSession {
    bool active = false;
    // Attach한 디버거 프로세스 - 디버거가 세션을 쥔 채로 먼저 죽어도
    // (예: 비정상 종료) 이 세션을 향한 강한 참조가 되지 않도록
    // WeakPtr(세션 자체의 소유자는 디버기 Process 자신이다 - 디버거가
    // 사라졌다고 세션까지 자동으로 정리되지는 않는다, 명시적
    // DebugDetach가 없으면 그대로 남아 §7 "디버기당 세션 1개"를
    // 계속 점유한다 - 이 자체가 §7이 의도한 안티 디버깅 속성의 일부:
    // 디버거가 죽었다고 다른 디버거가 끼어들 수 있게 조용히 풀리지
    // 않는다).
    WeakPtr<Process> debuggerProcess;
    DebugBreakpoint breakpoints[kMaxDebugBreakpoints];

    // [신규, 2026-09-17, SP-245D130B §9-4 답변("정지 사유 구분 플래그를
    // 둬야해")] §3.4/§3.5(브레이크포인트/싱글스텝/kSpawnDebugStart,
    // 전부 아직 미착수)가 이 디버기를 `TaskState::Blocked`로 세울 때
    // 함께 세워야 하는 "이 정지는 디버그 사유다"라는 표시 -
    // `ResourceGroup::thaw()`(resource_group.cpp)가 그룹 freeze를
    // 풀면서 실수로 디버그 정지까지 같이 풀어버리지 않도록 이 값을
    // 확인한다(`Process::frozenByGroup`과 대칭 - 그쪽은 "그룹이 나를
    // 세웠다", 이건 "디버거가 나를 세웠다"). **아직 아무도 이 값을
    // true로 세우지 않는다** - §3.4/§3.5 착수 세션이 실제로 Blocked로
    // 전환하는 지점(및 `kSpawnDebugStart` 소비 지점, process.cpp
    // `SpawnProcessHandler::onExec`)에서 함께 세워야 한다. 반대 방향
    // (`DebugContinue`가 그룹 freeze까지 실수로 풀어버리는 경우)은
    // `DebugContinue` 구현 시점에 `!proc->group->frozen`을 먼저
    // 확인해야 한다 - 이 주석이 그 요구사항을 미리 남겨 둔다.
    //
    // [갱신, 2026-09-19, PN-EA968DF0] plain bool에서 `Atomic<uint32_t>`로
    // 승격 - `kHandleUserBreakpointHit()`(임의 코어의 #DB ISR)가 쓰고
    // `Scheduler::kSyncDebugRegs()`/`onTick()`(다른 코어일 수 있음)가
    // 읽는, 코어를 가로지르는 진짜 데이터 레이스였다(plain bool은
    // 컴파일러/CPU 재정렬을 막을 방법이 없다) - `channel.h`의
    // `PendingConnectRequest::done`과 동일한 release-store/acquire-load
    // 패턴(release-acquire 페어링이 그 이전 쓰기까지 가시성을 보장하므로
    // 이 필드 자신 외 다른 필드를 추가로 원자화할 필요는 없다).
    Atomic<uint32_t> pausedByDebugger{0};

    // [갱신, 2026-09-19, PN-06A7C439] `singleStepPending`/`savedRegisters`/
    // `liveFramePtr`은 여기 process-wide로 두지 않는다 - `pausedByDebugger`
    // 는 프로세스 전체를 한꺼번에 세우는 all-stop 플래그라 그대로 두는
    // 게 맞지만("§7 진짜 갭" 분석은 PN-06A7C439 본문 참고), 여러 스레드가
    // 각자 다른 순간에 실제로 정지해 들어올 수 있게 된 이상 "정지된 그
    // 순간의 값" 자체는 스레드마다 독립이어야 한다 - `UserThread::
    // debugSavedRegisters`/`debugLiveFramePtr`/`debugSingleStepPending`
    // (syscall.h)로 옮겼다.
};

struct DebugAttachArgs {
    int64_t targetProcessId = -1;
    // out
    ChannelError error = ChannelError::None;
};

struct DebugDetachArgs {
    int64_t targetProcessId = -1;
    // out
    ChannelError error = ChannelError::None;
};

// [갱신, 2026-09-19, PN-06A7C439] `CreateThread`가 실제로 착수된 뒤에도
// targetThread를 **여전히 넣지 않는다** - 옛 주석의 예고("착수되면
// 추가한다")와 달리, 실제 코드 조사(kSyncDebugRegs, scheduler.cpp) 결과
// 이 필드는 애초부터 스레드별이 아니라 process-wide가 맞는 설계임이
// 확정됐다: DR0-3 하드웨어 슬롯 수(4개)와 정확히 일치하고
// (`kMaxDebugBreakpoints` 문서 주석 참고), `kSyncDebugRegs()`도 매
// 디스패치마다 "그 스레드가 속한 프로세스"의 브레이크포인트를 무조건
// 다시 싣는다 - 스레드마다 다른 브레이크포인트를 걸 수 있게 하려면
// DR 레지스터 자체를 스레드별로 가상화해야 하는데 하드웨어가 프로세스당
// 4개뿐이라 그럴 수 없다.
struct DebugSetBreakpointArgs {
    int64_t targetProcessId = -1;
    uint32_t slot = 0;  // 0..kMaxDebugBreakpoints-1
    uint64_t address = 0;
    DebugBreakpoint::Condition condition = DebugBreakpoint::Condition::Execute;
    bool enable = false;
    // out
    ChannelError error = ChannelError::None;
};

// [갱신, 2026-09-19, PN-06A7C439] `targetThread` 추가 - 여러 스레드가
// 동시에 정지해 있을 수 있게 된 이상(all-stop, `DebugSession::
// pausedByDebugger` 문서 주석 참고) "어느 스레드"의 다음 재개에
// 싱글스텝을 적용할지 반드시 구분해야 한다. 이 호출 자체는 재개하지
// 않고 그 스레드의 `UserThread::debugSingleStepPending`(syscall.h)만
// 세우거나 내린다 - 실제 RFLAGS.TF 반영은 그다음 `DebugContinue`가
// write-back할 때 한다.
struct DebugSetSingleStepArgs {
    int64_t targetProcessId = -1;
    ThreadId targetThread = kInvalidThreadId;
    bool enable = false;
    // out
    ChannelError error = ChannelError::None;
};

// [갱신, 2026-09-19, PN-06A7C439] targetThread를 넣지 않는다(위
// DebugSetSingleStepArgs와 반대 결론) - `DebugContinue`는 이 프로세스의
// 정지된 스레드 **전부**(`UserThread::debugLiveFramePtr != nullptr`인
// 스레드 전부)를 한 번에 write-back하고 프로세스 전체를 재개하는
// all-stop→continue-all 시맨틱을 그대로 유지한다("이 스레드만 재개"하는
// 선택적 재개는 이 계획(PN-06A7C439) 범위 밖 - 필요해지면 별도 계획).
struct DebugContinueArgs {
    int64_t targetProcessId = -1;
    // out
    ChannelError error = ChannelError::None;
};

// [신규, 2026-09-17, SP-9A6D579F §3.5] 디버기의 [address, address+length)
// 구간을 커널이 direct map 경유로 대행 복사해 디버거의 `out` 버퍼(호출자
// 자신의 유저 포인터)에 채운다 - 디버기 주소공간을 디버거 쪽에 매핑하지
// 않는다(설계 그대로).
struct DebugReadMemoryArgs {
    int64_t targetProcessId = -1;
    uint64_t address = 0;
    uint64_t length = 0;
    void* out = nullptr;  // 유저 포인터(호출자=디버거 소유 버퍼)
    // out
    ChannelError error = ChannelError::None;
};

// DebugReadMemoryArgs와 대칭 - 디버거의 `in` 버퍼(호출자 자신의 유저
// 포인터)에서 읽어 디버기의 [address, address+length) 구간에 대행
// 복사로 써 넣는다.
struct DebugWriteMemoryArgs {
    int64_t targetProcessId = -1;
    uint64_t address = 0;
    uint64_t length = 0;
    const void* in = nullptr;  // 유저 포인터(호출자=디버거 소유 버퍼)
    // out
    ChannelError error = ChannelError::None;
};

// [갱신, 2026-09-19, PN-06A7C439] `targetThread` 추가 - DebugSetSingleStepArgs
// 와 동일한 이유(여러 스레드가 동시에 정지해 있을 수 있어 "어느
// 스레드"인지 구분 필요). `out`은 호출자(디버거) 소유의
// `DebugRegisterSnapshot` 버퍼 - 대상 스레드가 정지 상태
// (`UserThread::debugLiveFramePtr != nullptr`)가 아니면 `NotFound`.
struct DebugGetRegistersArgs {
    int64_t targetProcessId = -1;
    ThreadId targetThread = kInvalidThreadId;
    DebugRegisterSnapshot* out = nullptr;  // 유저 포인터(호출자=디버거 소유 버퍼)
    // out
    ChannelError error = ChannelError::None;
};

// DebugGetRegistersArgs와 대칭(targetThread 포함) - `in`에서 읽어 대상
// 스레드의 살아있는 `*UserThread::debugLiveFramePtr`(syscall.h)에
// 그 자리에서 바로 반영한다(값은 이미 진짜 자리에 있다 - 더 이상
// 별도 사본→write-back 단계가 없다, QU-47A83CDF 답변 반영). 이 호출
// 자체는 재개하지 않는다 - 그룹 freeze 여부와 무관하게 별도로
// DebugContinue를 불러야 실제로 재개된다(DebugContinue는 RFLAGS.TF/RF
// 보정만 마저 한다, `kWriteBackDebugFrame()` 참고).
struct DebugSetRegistersArgs {
    int64_t targetProcessId = -1;
    ThreadId targetThread = kInvalidThreadId;
    const DebugRegisterSnapshot* in = nullptr;  // 유저 포인터(호출자=디버거 소유 버퍼)
    // out
    ChannelError error = ChannelError::None;
};

class DebugSessionService {
public:
    // 부팅 시 한 번 - 위 syscall들을 SyscallRegistry에 등록한다.
    static void registerSyscallEndpoints();

    // [신규, 2026-09-17, SP-9A6D579F §4] 부팅 시 한 번 -
    // Idt::registerDebugCallback()으로 #DB ISR 소비자를 등록한다.
    // registerSyscallEndpoints()와 별도 함수로 분리한 이유는 하나가
    // SyscallRegistry, 다른 하나가 Idt라는 완전히 다른 등록 대상을
    // 다루기 때문(호출부는 둘 다 kmain.cpp에서 순서 무관하게 부른다).
    static void registerDebugCallback();
};

// [신규, 2026-09-17, SP-9A6D579F §3.5, resource_group.h의
// kCheckAndMarkFrozen()과 대칭] Scheduler::onTick()의 재스케줄 결정
// 지점에서 호출한다 - task가 유저 프로세스에 속하고 그 디버그 세션이
// 방금 그 task를 정지시켰으면(#DB 콜백이 pausedByDebugger를 세워
// 뒀으면) true. 커널 전용 Task나 디버그 세션이 없으면 항상 false.
// kCheckAndMarkFrozen()과 달리 이 함수 자신은 아무것도 "세우지"
// 않는다 - 세우는 주체는 #DB 콜백(kHandleUserBreakpointHit,
// debug_session.cpp)이고, 이 함수는 그 결과를 그저 읽기만 한다.
bool kIsPausedByDebugger(Task* task);

// [신규, 2026-09-17, SP-9A6D579F §3.5, DC-47000304 (A) 채택]
// `Scheduler::onTick()`이 `kIsPausedByDebugger(task)`가 true라
// `task`를 `Blocked`로 남기기로 결정한 바로 그 지점에서, 그 결정에
// 쓰인 것과 같은 `frame`(이 코어가 방금 EOI를 보낸 스케줄러 틱
// 자신의 `InterruptFrame*` - `task` 자신의 커널 스택 위, isr_common_stub
// 이 쌓아 둔 자리)을 넘겨 호출한다. `task`가 유저 프로세스에 속하고
// 활성 디버그 세션이 있으면 `DebugSession::savedRegisters`(값 복사)
// 와 `liveFramePtr`(그 살아있는 프레임의 주소, write-back용)를 채운다
// - 그 외(커널 전용 Task, 세션 없음)엔 아무 일도 하지 않는다(방어적).
void kSaveDebugRegistersSnapshot(Task* task, InterruptFrame* frame);

}  // namespace kernel

#endif  // MINICORE_KERNEL_DEBUG_SESSION_H
