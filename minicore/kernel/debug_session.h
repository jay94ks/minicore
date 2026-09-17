#ifndef MINICORE_KERNEL_DEBUG_SESSION_H
#define MINICORE_KERNEL_DEBUG_SESSION_H

#include "channel.h"
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
// [SP-9A6D579F §3.4] DebugSetSingleStep(call 3) - RFLAGS.TF가 코어
// 레지스터가 아니라 그 Task 자신의 저장된 인터럽트 프레임/popfq 값
// 안에 있어(kSyncDebugRegs류 "디스패치 시점에 다시 쓰기" 패턴을 그대로
// 못 씀) 그 프레임을 어디서 찾아 고칠지 별도 설계가 필요하다 - 번호만
// 예약, 미구현(PN-87D6B615 "남은 범위" 참고).
//
// [신규, 2026-09-17, SP-9A6D579F §3.5/§4, PN-87D6B615, RM-48E1E610
// 7.4/7.7/7.8] 이번 증분(항목5 일부/6)이 구현하는 syscall -
// `DebugContinue`(레지스터 접근 불필요, 그룹 freeze 교차 확인만
// 필요)와 `DebugReadMemory`/`DebugWriteMemory`(대상 주소공간을
// Paging::translatePage()로 직접 순회하는 커널 대행 복사 - 레지스터
// 프레임 위치와 무관). `DebugGetRegisters`/`DebugSetRegisters`(call
// 5/6)는 DebugSetSingleStep과 동일한 이유로 미구현 - "정지된 유저
// Task의 전체 레지스터 상태(콜러세이브 아닌 것 포함)가 정확히 그
// 커널 스택의 어느 InterruptFrame에 있는지"가 아직 설계되지 않은
// 자리라(SP-83A07867이 다루는 건 CR3/RSP0/FPU/디버그 레지스터처럼
// "코어 소유" 상태뿐, "이 Task가 트랩 당시 갖고 있던 전체 유저
// 레지스터 스냅숏의 정확한 스택 오프셋"은 아직 어떤 문서도 확정한
// 적이 없음) - DC-47000304/QU-C82E903B로 별도 질의 등록, 설계자
// 답변 대기 중(아래 참고).
constexpr SyscallEndpointId kSyscallEndpointDebugContinue = kMakeSyscallEndpointId(7, 4);
constexpr SyscallEndpointId kSyscallEndpointDebugReadMemory = kMakeSyscallEndpointId(7, 7);
constexpr SyscallEndpointId kSyscallEndpointDebugWriteMemory = kMakeSyscallEndpointId(7, 8);

// [SP-9A6D579F §3.1] DR0-DR3 하드웨어 슬롯 수와 동일 - 스레드마다
// 별도 슬롯이 아니라 프로세스당(사실상 mainThread 고정, 멀티스레드
// 디버깅은 PN-2E4E9D79 완료 전까지 범위 밖) 공유.
constexpr uint32_t kMaxDebugBreakpoints = 4;

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
// 않는다 - §1-A(멀티스레드 유저 프로세스 지원, PN-2E4E9D79)가 아직
// 없어 "프로세스당 스레드 하나"가 사실상 불변조건이므로, 그 필드가
// 있어도 항상 mainThread 고정일 수밖에 없다(과설계 방지,
// RM-23F4B687 §4) - 그 계획이 완료되면 이 struct에 추가한다.
struct DebugSetBreakpointArgs {
    int64_t targetProcessId = -1;
    uint32_t slot = 0;  // 0..kMaxDebugBreakpoints-1
    uint64_t address = 0;
    DebugBreakpoint::Condition condition = DebugBreakpoint::Condition::Execute;
    bool enable = false;
    // out
    ChannelError error = ChannelError::None;
};

// [신규, 2026-09-17, SP-9A6D579F §3.5] targetThread 없음 - 위
// DebugSetBreakpointArgs와 동일한 이유(PN-2E4E9D79 완료 전까지
// mainThread 고정).
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

}  // namespace kernel

#endif  // MINICORE_KERNEL_DEBUG_SESSION_H
