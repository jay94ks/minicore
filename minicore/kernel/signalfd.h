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
// **표준 신호(1-31)**: 같은 번호가 여러 번 도착해도 "펜딩" 여부만
// 비트로 추적(POSIX 표준 시그널과 동일한 coalescing). `Read`가
// 반환하는 `SignalfdSiginfo`도 POSIX `signalfd_siginfo`의 ~30개 필드
// 중 `ssi_signo`(신호 번호) 하나만 담는 최소 부분집합 - pid/uid/status
// 등 나머지는 이 커널에 그 정보를 채울 인프라 자체가 아직 없어 후속
// (§5 참고).
//
// **[추가, 2026-09-29, PN-FD706AF6] RT 신호(32-63)**: 표준 신호와
// 완전히 다른 체계 - `Process::signalMask`/`pendingSignals`/체크포인트
// 를 전혀 거치지 않고(signal.h `kRtSignalBase` 문서 주석 참고), 같은
// 번호가 여러 번 와도 전부 별도 인스턴스로 FIFO 큐(`RtSignalQueue`,
// `EventPendingQueue`와 동일한 고정 용량 N=8 + 가득 차면 가장 오래된
// 것부터 버리는 관례)에 쌓인다 - `Process::raiseRtSignal()`이 직접
// 채운다(`Process::raiseSignal()`과는 별개 경로).
namespace kernel {

class Process;  // 포인터로만 참조(SignalfdState::ownerProcess) - 전체 정의는 process.h

// [신규, 2026-09-29, PN-FD706AF6, SP-A7479F83 §6-B] RT 신호 인스턴스
// 하나 - 설계 원안 그대로.
struct RtSignalInstance {
    uint32_t signalNumber = 0;  // kRtSignalBase(32)..kRtSignalMax(63)
    uint64_t userData = 0;      // sigqueue()류 부가 데이터(1워드로 축소, v1 단순화)
};

// event_topic.h의 EventPendingQueue와 동일한 고정 용량 링 버퍼 -
// 가득 차면 가장 오래된 것부터 버린다(SP-A7479F83 §6-B가 명시한 정책,
// "새 정책을 또 만들지 않는다"는 그 문서 자신의 판단 그대로 재사용).
class RtSignalQueue {
public:
    static constexpr uint32_t kCapacity = 8;

    void push(const RtSignalInstance& instance) {
        if (_count < kCapacity) {
            const uint32_t tail = (_head + _count) % kCapacity;
            _entries[tail] = instance;
            ++_count;
        } else {
            _entries[_head] = instance;
            _head = (_head + 1) % kCapacity;
        }
    }

    bool pop(RtSignalInstance* outInstance) {
        if (_count == 0) {
            return false;
        }
        *outInstance = _entries[_head];
        _head = (_head + 1) % kCapacity;
        --_count;
        return true;
    }

    uint32_t count() const { return _count; }

private:
    RtSignalInstance _entries[kCapacity]{};
    uint32_t _head = 0;
    uint32_t _count = 0;
};

struct SignalfdState {
    Spinlock lock;
    uint32_t watchedSignalMask = 0;  // 표준 신호(1-31) 관심 집합(SignalfdSetMask가 갱신)
    uint32_t pendingMask = 0;        // bit n = SignalNumber n 도착, 아직 이 fd로 안 읽음
    // [신규, 2026-09-29, PN-FD706AF6] RT 신호(32-63) 관심 집합 - bit n
    // = 신호 번호 (kRtSignalBase+n) 관심. 표준 신호와 별도 필드인 이유는
    // 두 체계가 서로 다른 저장 방식(비트 vs FIFO)이라 켜고 끄는 것도
    // 독립적이어야 하기 때문(signal.h kRtSignalBase 문서 주석 참고).
    uint32_t watchedRtMask = 0;
    RtSignalQueue rtQueue;
    WeakPtr<Process> ownerProcess;
    int32_t ownerFd = -1;
    AsyncTaskWaitQueue pendingReaders;
    EpollObserverQueue epollReadObservers;
    void destroy() {}
};

constexpr SyscallEndpointId kSyscallEndpointSignalfdCreate = kMakeSyscallEndpointId(6, 8);
constexpr SyscallEndpointId kSyscallEndpointSignalfdSetMask = kMakeSyscallEndpointId(6, 9);

struct SignalfdCreateArgs {
    uint32_t signalMask = 0;    // in - 초기 표준 신호(1-31) 관심 집합(bit n = SignalNumber n)
    uint32_t rtSignalMask = 0;  // in - 초기 RT 신호(32-63) 관심 집합(bit n = 신호 kRtSignalBase+n)
    // out
    ChannelError error = ChannelError::None;  // InvalidArgument(Kill/Stop 비트 포함 시 - 마스킹 불가 원칙)
    int64_t fd = -1;
};

struct SignalfdSetMaskArgs {
    int32_t fd = -1;
    uint32_t signalMask = 0;    // in - 새 표준 신호 관심 집합(SetMask 의미 - 통째로 교체)
    uint32_t rtSignalMask = 0;  // in - 새 RT 신호 관심 집합(마찬가지로 통째로 교체)
    // out
    ChannelError error = ChannelError::None;  // InvalidHandle/InvalidArgument
};

// POSIX signalfd_siginfo의 최소 부분집합(문서 상단 주석 참고).
struct SignalfdSiginfo {
    uint32_t signo = 0;
    // [신규, 2026-09-29, PN-FD706AF6] RT 신호를 읽었을 때만 유효 -
    // 표준 신호는 항상 0.
    uint64_t userData = 0;
};

class Signalfd {
public:
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SIGNALFD_H
