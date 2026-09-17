#ifndef MINICORE_KERNEL_SIGNAL_H
#define MINICORE_KERNEL_SIGNAL_H

#include "channel.h"
#include "libkenv/chunked_list.h"
#include "libkenv/types.h"
#include "syscall.h"

namespace kernel {

// Minicore Signal 번호(RM-B5764185 "정본" 그대로, SP-0666DB3C §4.2) -
// 0=무효, 1~22/31=POSIX 표준과 동일한 번호. 새 신호가 필요해지면 이
// enum이 아니라 RM-B5764185에 먼저 번호를 예약한다(CLAUDE.md 규칙 13).
enum class SignalNumber : uint32_t {
    None = 0,          // 무효값 - 신호 없음

    Hup = 1,           // SIGHUP  - 터미널 세션 종료(주로 설정 리로드용)
    Int = 2,           // SIGINT  - 키보드 인터럽트(Ctrl+C)
    Quit = 3,          // SIGQUIT - 키보드 종료(Ctrl+\), 코어 덤프
    IllegalInstruction = 4,  // SIGILL  - 잘못된 머신 명령어(#UD 트랩 시 커널이 발생)
    Trap = 5,          // SIGTRAP - 디버깅 브레이크포인트 등
    Abort = 6,         // SIGABRT - abort() 등 비정상 종료 호출
    Bus = 7,           // SIGBUS  - 잘못된 메모리 주소 접근
    Fpe = 8,           // SIGFPE  - 산술 연산 오류(0 나누기 등)
    Kill = 9,          // SIGKILL - 즉시 강제 종료, 마스킹/무시 불가
    Usr1 = 10,         // SIGUSR1 - 사용자 정의 1
    Segv = 11,         // SIGSEGV - 잘못된 메모리 참조(유저 폴트 처리 §2-B가 커널이 스스로 발생)
    Usr2 = 12,         // SIGUSR2 - 사용자 정의 2
    Pipe = 13,         // SIGPIPE - 읽는 쪽 없는 파이프에 쓰기
    Alrm = 14,         // SIGALRM - 알람 타이머 만료
    Terminate = 15,    // SIGTERM - 기본 종료 신호, 핸들러로 가로챌 수 있음
    // 16번은 POSIX SIGSTKFLT 등에 대응하는 자리 - v1은 정의하지 않음(RM-B5764185)
    Chld = 17,         // SIGCHLD - 자식 프로세스 종료/중지 통지
    Cont = 18,         // SIGCONT - 중지된 프로세스 재개
    Stop = 19,         // SIGSTOP - 일시 중지, 마스킹/무시 불가
    Tstp = 20,         // SIGTSTP - 터미널 정지 신호(Ctrl+Z)
    Ttin = 21,         // SIGTTIN - 백그라운드 프로세스의 터미널 입력 시도
    Ttou = 22,         // SIGTTOU - 백그라운드 프로세스의 터미널 출력 시도
    // 23~30번은 v1 범위 밖(RM-B5764185) - 필요해지면 그 표에 먼저 예약
    Sys = 31,          // SIGSYS  - 잘못된 시스템 콜 요청
};

// RM-B5764185의 최대 번호(31) 기준 - 인덱스로 쓰는 dispositions[]가
// 0..31 전부를 담을 수 있으면 된다(23~30 미사용도 자리만 차지, 상수
// 하나로 단순하게 유지).
constexpr uint32_t kSignalCount = 32;

// v1이 실제로 발생시키는 것은 이 중 4개뿐(Kill/Terminate/Segv/
// IllegalInstruction, RM-B5764185 "v1에서 실제로 발생/처리하는 신호"
// 절) - 나머지 번호는 enum에 자리만 예약돼 있다.

enum class SignalDisposition : uint32_t {
    Default,   // 기본 동작(대부분 프로세스 종료) - v1의 유일하게 실제
               // 구현 가능한 값(핸들러 호출 인프라가 없으므로)
    Ignore,    // 무시 - Kill/Stop에는 적용 불가(마스킹 불가 원칙)
    Handler,   // 유저 핸들러로 위임 - PN-124C105B 완료 전까지는
               // 등록만 받아 두고 실제 호출은 하지 않는다(SP-0666DB3C §4.4)
};

// Process에 매달린 대기 중 신호 하나(SP-0666DB3C §4.3 그대로) -
// ChunkedList<PendingSignal, N>이 여러 개를 담는다(DmaBuffer,
// SP-39F18E30 §4와 동일 패턴 - 단순 배열 대신 재사용 가능한 GENERIC
// 컨테이너).
struct PendingSignal {
    SignalNumber number = SignalNumber::None;
    bool used = false;
};

// 청크 용량 - PendingSignal이 UserThread::PendingSyscall보다 작아
// (SignalNumber 4B + bool, AsyncTaskManageCode 8B 필드가 없음) 같은
// 슬랩 버킷에 더 많이 들어간다. 정확한 최적값은 실측 후 조정 가능한
// 구현 세부(RM-23F4B687 §4) - 지금은 UserThread::kPendingSyscallChunkCapacity
// 와 같은 자릿수로 시작.
constexpr uint32_t kPendingSignalChunkCapacity = 10;

// [갱신, 2026-09-17, SP-E9B44929] Process 그룹(0) - SP-0666DB3C §4.5
// Syscall API.
constexpr SyscallEndpointId kSyscallEndpointKill = kMakeSyscallEndpointId(0, 1);
constexpr SyscallEndpointId kSyscallEndpointSignalAction = kMakeSyscallEndpointId(0, 2);

// [SP-0666DB3C §4.5, PN-71E50394 항목 4] `Kill(targetProcessId, signal)` -
// `targetProcessId`는 `SpawnProcessArgs::pid`/`WaitArgs::targetPid`와
// 동일한 관례(대상 `Process*`를 `reinterpret_cast<int64_t>`한 값).
//
// **[v1 잠정 범위, 2026-09-17]** SP-0666DB3C §11 항목4("프로세스 ID
// 체계 - PN-268F062B와 통일 예정, 아직 열려 있는 설계 영역")가 실제
// 구현 시점까지 미확정으로 남겨 둔 부분 - 임의의 `targetProcessId`를
// 검증 없이 `reinterpret_cast`해 역참조하면 Channel/Bridge에서 이미
// 겪은 것과 같은 임의 포인터 역참조 보안 공백이 되므로, v1은 **호출자
// 자신의 직계 자식(`Process::children`)만** 대상으로 허용한다(`Wait`
// syscall과 정확히 같은 스코프/검증 방식 - `WaitHandler::onExec` 참고).
// 자식이 아닌 값(위조/타 프로세스/조부모 등)은 전부 `NotFound`로
// 거부된다 - 자기 자신도 대상이 될 수 없다(자기 자신은 `children`에
// 없음). 부모가 자식 이외의 임의 프로세스에 신호를 보내야 하는
// 시나리오(예: 특권 관리 프로세스가 무관한 프로세스를 종료)가 실제로
// 필요해지면, 그건 이 v1 스코프를 넘어서는 새 설계 결정이라 별도 DC/
// 질의로 확인 후 넓힌다(CLAUDE.md 규칙 4) - 지금 임의로 넓히지 않는다.
struct KillArgs {
    int64_t targetProcessId = -1;
    SignalNumber signal = SignalNumber::None;
    // out
    ChannelError error = ChannelError::None;
};

// [SP-0666DB3C §4.5, PN-71E50394 항목 4] `SignalAction(signal,
// disposition)` - 호출자 자신의 `Process::dispositions[]`만 바꾼다
// (다른 프로세스의 처리 방식을 원격으로 바꾸는 API는 없음 - 각
// 프로세스가 자기 자신의 신호 처리 방식만 스스로 설정하는 POSIX
// `sigaction()`과 동일한 범위). `Kill`/`Stop`을 `Ignore`로 설정하려는
// 시도는 `InvalidArgument`로 거부한다(signal.h `SignalDisposition`
// 문서 주석의 "마스킹 불가 원칙" 그대로). `Handler`는 §4.4 조건
// (PN-124C105B, 유저 핸들러 실제 ring3 호출 인프라)이 아직 없어
// `NotSupported`로 거부한다 - 등록만 받아 두고 조용히 무시하지
// 않는다(표준 커널 syscall 관례, SpawnProcess의 flags 검증과 동일한
// 취지).
struct SignalActionArgs {
    SignalNumber signal = SignalNumber::None;
    SignalDisposition disposition = SignalDisposition::Default;
    // out
    ChannelError error = ChannelError::None;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SIGNAL_H
