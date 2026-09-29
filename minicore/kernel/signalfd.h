#ifndef MINICORE_KERNEL_SIGNALFD_H
#define MINICORE_KERNEL_SIGNALFD_H

#include "channel.h"  // ChannelError, AsyncTaskWaitQueue, EpollObserverQueue 재사용(timerfd.h와 동일 관례)
#include "libkenv/types.h"
#include "syscall.h"

// signalfd(SP-A7479F83 §2/§3/§4, PN-A1A0B595) - epoll 호환 시그널 fd.
// timerfd.h와 완전히 같은 구조(Create/SetMask + 그룹3 Read 재사용 +
// epoll 통합) - 차이는 "만료 횟수" 대신 "관심 시그널 집합 중 도착한
// 것들의 비트마스크"를 펜딩 상태로 든다는 것뿐이다.
//
// **§4의 핵심 결정 - SignalfdSetMask는 SignalMask(Block)도 함께 건다**:
// SignalfdCreate/SignalfdSetMask가 지정한 시그널은 SP-0666DB3C §4.6의
// 프로세스 시그널 마스크에도 동시에 블록된다(Process::signalMask 직접
// 조작, syscall 왕복 없이 커널 내부 헬퍼로) - 그래야 그 시그널이
// kCheckSignalCheckpoint()의 일반 종료 경로로 새지 않고 이 fd로만
// 도착한다(POSIX signalfd()의 표준 패턴, §4 원안 그대로).
//
// **v1 스코프**: 표준 시그널(1-31)만 다룬다 - 같은 번호가 여러 번
// 도착해도 "펜딩" 여부만 비트로 추적(POSIX 표준 시그널과 동일한
// coalescing, RT 신호 큐잉은 PN-FD706AF6로 완전히 분리된 후속 범위,
// SP-A7479F83 §6-B). `Read`가 반환하는 `SignalfdSiginfo`도 POSIX
// `signalfd_siginfo`의 ~30개 필드 중 `ssi_signo`(신호 번호) 하나만
// 담는 최소 부분집합 - pid/uid/status 등 나머지는 이 커널에 그 정보를
// 채울 인프라 자체가 아직 없어 후속(§5 참고).
namespace kernel {

class Process;  // 포인터로만 참조(SignalfdState::ownerProcess) - 전체 정의는 process.h

struct SignalfdState {
    Spinlock lock;
    uint32_t watchedSignalMask = 0;  // 이 fd가 관심 있는 시그널 집합(SignalfdSetMask가 갱신)
    uint32_t pendingMask = 0;        // bit n = SignalNumber n 도착, 아직 이 fd로 안 읽음
    WeakPtr<Process> ownerProcess;
    int32_t ownerFd = -1;
    AsyncTaskWaitQueue pendingReaders;
    EpollObserverQueue epollReadObservers;
    void destroy() {}
};

constexpr SyscallEndpointId kSyscallEndpointSignalfdCreate = kMakeSyscallEndpointId(6, 8);
constexpr SyscallEndpointId kSyscallEndpointSignalfdSetMask = kMakeSyscallEndpointId(6, 9);

struct SignalfdCreateArgs {
    uint32_t signalMask = 0;  // in - 초기 관심 시그널 집합(SignalNumber 비트마스크, bit n = SignalNumber n)
    // out
    ChannelError error = ChannelError::None;  // InvalidArgument(Kill/Stop 비트 포함 시 - 마스킹 불가 원칙)
    int64_t fd = -1;
};

struct SignalfdSetMaskArgs {
    int32_t fd = -1;
    uint32_t signalMask = 0;  // in - 새 관심 시그널 집합(SetMask 의미 - 통째로 교체)
    // out
    ChannelError error = ChannelError::None;  // InvalidHandle/InvalidArgument
};

// POSIX signalfd_siginfo의 최소 부분집합(문서 상단 주석 참고).
struct SignalfdSiginfo {
    uint32_t signo = 0;
};

class Signalfd {
public:
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SIGNALFD_H
