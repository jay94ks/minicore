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
// 그냥 대기한다). `kSaveDebugRegistersSnapshot()`이 이 프레임의
// 값을 `DebugSession::savedRegisters`에 복사해 두고(값 복사 -
// 여러 syscall/다른 코어에서 안전하게 조회할 수 있도록), 동시에
// 그 살아있는 프레임 자신의 주소도 `DebugSession::liveFramePtr`에
// 남겨 둔다(내부 전용, 어떤 syscall args에도 노출 안 됨) -
// `DebugSetRegisters`는 이 사본만 바꾸고, `DebugContinue`가 재개
// 직전 이 사본 값을 `*liveFramePtr`에 다시 써넣어(write-back) 실제
// iretq 프레임에 반영한다.
constexpr SyscallEndpointId kSyscallEndpointDebugGetRegisters = kMakeSyscallEndpointId(7, 5);
constexpr SyscallEndpointId kSyscallEndpointDebugSetRegisters = kMakeSyscallEndpointId(7, 6);

// [SP-9A6D579F §3.1] DR0-DR3 하드웨어 슬롯 수와 동일 - 스레드마다
// 별도 슬롯이 아니라 프로세스당 공유. [갱신, 2026-09-18, PN-0EB2FABF
// (구 PN-2E4E9D79)] "프로세스당 스레드 하나뿐"이라는 옛 전제(`Process::
// mainThread`)는 SP-76250478로 걷어냈지만, 이 `DebugSession` 자체는
// 여전히 스레드 구분 없이 프로세스 전체에 하나뿐이다 - 진짜 멀티스레드
// 디버깅(스레드별 브레이크포인트/레지스터)은 그 소비자인 SP-9A6D579F
// §1-A/§3.4/§3.5의 targetThread 파라미터가 실제로 추가될 때까지 범위
// 밖으로 남는다(아래 각 Debug*Args 문서 주석도 동일).
constexpr uint32_t kMaxDebugBreakpoints = 4;

// [신규, 2026-09-17, SP-9A6D579F §3.5, DC-47000304] `InterruptFrame`
// 자체를 syscall ABI로 그대로 노출하지 않는다 - `vector`/`errorCode`는
// ISR 자신의 장부일 뿐 "레지스터"가 아니다(디버거 입장에서 의미 없는
// 필드를 읽고 쓰게 하지 않기 위한 최소 API 위생, RM-23F4B687 §4). 그
// 외 필드는 `InterruptFrame`과 정확히 같은 이름/순서 - `kSaveDebugRegistersSnapshot()`
// 이 필드별로 복사한다.
struct DebugRegisterSnapshot {
    uint64_t rax = 0, rbx = 0, rcx = 0, rdx = 0, rsi = 0, rdi = 0, rbp = 0;
    uint64_t r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;
    uint64_t rip = 0;
    uint64_t cs = 0;
    uint64_t rflags = 0;
    uint64_t rsp = 0;
    uint64_t ss = 0;
};

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
    // [구현 완료, 2026-09-17, SP-9A6D579F §3.4] `DebugSetSingleStep`이
    // 세우는(또는 내리는) 요청 플래그 - 이름 그대로 "한 번 쓰이면
    // 소비되는" 값이다. `DebugContinue`가 재개 직전 write-back할 때
    // 이 값이 true면 `savedRegisters.rflags`의 TF 비트(0x100)를 세운
    // 뒤 이 플래그를 즉시 false로 되돌리고(한 번의 DebugSetSingleStep
    // 호출은 정확히 한 번의 다음 DebugContinue에만 적용), false면 TF
    // 비트를 강제로 지운다(정지 사유가 싱글스텝 트랩 자신이었을 때
    // `savedRegisters.rflags`에 TF=1이 그대로 남아 있어 그걸 그냥
    // 되쓰면 무한 싱글스텝에 빠지는 것을 막는다).
    bool singleStepPending = false;

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
    bool pausedByDebugger = false;

    // [신규, 2026-09-17, SP-9A6D579F §3.5, DC-47000304 (A) 채택]
    // `kSaveDebugRegistersSnapshot()`이 `pausedByDebugger`를 세우는
    // 바로 그 순간(Scheduler::onTick()) 함께 채운다 - `DebugGetRegisters`/
    // `DebugSetRegisters`는 이 값 복사본만 읽고 쓴다. `liveFramePtr`는
    // 그 값이 실려 있던 진짜 살아있는 `InterruptFrame`(이 디버기 자신의
    // 커널 스택 위, 아직 그 자리에 그대로 있음)의 주소 - 어떤 syscall
    // args에도 노출하지 않는 내부 전용 필드로, `DebugContinue`가 재개
    // 직전 `savedRegisters`를 여기 다시 써넣어(write-back) 반영한
    // 뒤 즉시 `nullptr`로 되돌린다(재사용/댕글링 방지 - 이 Task가
    // 다시 정지하기 전까지는 무효).
    DebugRegisterSnapshot savedRegisters;
    InterruptFrame* liveFramePtr = nullptr;
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

// [신규, 2026-09-17, SP-9A6D579F §3.4] targetThread 파라미터는 넣지
// 않는다 - [갱신, 2026-09-18, PN-0EB2FABF(구 PN-2E4E9D79)]
// `Process::threads` 자료구조 자체는 여러 스레드를 담을 수 있게 됐지만,
// 실제로 두 번째 이상의 스레드를 만드는 `CreateThread` syscall이 아직
// 없어 "프로세스당 스레드 하나"가 여전히 사실상 불변조건이다 - 그
// 필드가 있어도 항상 그 유일한 스레드 고정일 수밖에 없다(과설계 방지,
// RM-23F4B687 §4) - `CreateThread`가 실제로 착수되면 이 struct에
// 추가한다.
struct DebugSetBreakpointArgs {
    int64_t targetProcessId = -1;
    uint32_t slot = 0;  // 0..kMaxDebugBreakpoints-1
    uint64_t address = 0;
    DebugBreakpoint::Condition condition = DebugBreakpoint::Condition::Execute;
    bool enable = false;
    // out
    ChannelError error = ChannelError::None;
};

// [구현 완료, 2026-09-17, SP-9A6D579F §3.4] targetThread 없음 - 위
// DebugSetBreakpointArgs와 동일한 이유. 이 호출 자체는 재개하지 않고
// `DebugSession::singleStepPending`만 세우거나 내린다 - 실제 RFLAGS.TF
// 반영은 그다음 `DebugContinue`가 write-back할 때 한다(debug_session.h
// 상단 `singleStepPending` 문서 주석 참고).
struct DebugSetSingleStepArgs {
    int64_t targetProcessId = -1;
    bool enable = false;
    // out
    ChannelError error = ChannelError::None;
};

// [신규, 2026-09-17, SP-9A6D579F §3.5] targetThread 없음 - 위
// DebugSetBreakpointArgs와 동일한 이유(`CreateThread`가 실제로
// 착수되기 전까지는 프로세스당 스레드가 여전히 하나뿐).
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

// [신규, 2026-09-17, SP-9A6D579F §3.5, DC-47000304] targetThread
// 없음(위 DebugContinueArgs와 동일한 이유). `out`은 호출자(디버거)
// 소유의 `DebugRegisterSnapshot` 버퍼 - 대상이 정지 상태(`pausedByDebugger`)
// 가 아니면 `NotFound`.
struct DebugGetRegistersArgs {
    int64_t targetProcessId = -1;
    DebugRegisterSnapshot* out = nullptr;  // 유저 포인터(호출자=디버거 소유 버퍼)
    // out
    ChannelError error = ChannelError::None;
};

// DebugGetRegistersArgs와 대칭 - `in`에서 읽어 대상의 `DebugSession::
// savedRegisters`에 반영한다. 실제로 재개 시(DebugContinue) 살아있는
// 프레임에 write-back된다(debug_session.h 상단 주석 참고) - 이 호출
// 자체는 재개하지 않는다(그룹 freeze 여부와 무관하게 항상 사본만
// 갱신, 별도로 DebugContinue를 불러야 함).
struct DebugSetRegistersArgs {
    int64_t targetProcessId = -1;
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
