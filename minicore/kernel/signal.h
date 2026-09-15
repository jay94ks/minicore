#ifndef MINICORE_KERNEL_SIGNAL_H
#define MINICORE_KERNEL_SIGNAL_H

#include "libkenv/chunked_list.h"
#include "libkenv/types.h"

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

}  // namespace kernel

#endif  // MINICORE_KERNEL_SIGNAL_H
