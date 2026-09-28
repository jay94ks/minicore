#ifndef MINICORE_KERNEL_TIMERFD_H
#define MINICORE_KERNEL_TIMERFD_H

#include "channel.h"  // ChannelError, AsyncTaskWaitQueue 재사용
#include "libkenv/types.h"
#include "syscall.h"

// timerfd(SP-A7479F83 §2/§3/§6-A) - epoll 호환 타이머 fd. v1 범위는
// 상대 틱 1회성/주기 타이머 + Create/SetTime뿐(절대시각/RT신호/
// signalfd는 범위 밖, PN-0F56DE4B 참고). Read(그룹3 기존 syscall
// 재사용, vfs_syscall.cpp)가 만료 횟수(8바이트)를 읽고 리셋한다.
//
// [PN-96265AE4로 발견+해소] 주기 타이머의 첫 블로킹 Read가 영원히
// 깨어나지 못하던 결함의 근본 원인은 `kOnTimerfdFire`(timerfd.cpp)가
// 대기자를 깨울 때 `AsyncReactor::submitCompletion()`을 기본값
// (preemptive=false)으로 호출한 것이었다 - PN-4FA5F13B가 이미 확립한
// 원칙("실제로 파킹된 대기자를 깨우는 경로는 preemptive=true로 즉시
// 드레인을 강제해야 한다", process.cpp의 JoinHandler 깨우기와 동일
// 패턴)을 놓쳤다. `kOnTimerfdFire`가 `DelayedExecutionQueue::pump()`
// (idle-fallback 경로)에서 호출되므로, 대기자의 homeCoreIndex가 지금
// pump()를 실행 중인 코어와 다르면 preemptive=false는 그 코어가 다음
// 스스로 idle이 될 때까지 깨우지 못하고(그 코어가 계속 바쁘면 영원히
// 못 깰 수도 있음) - preemptive=true로 그 코어에 즉시 IPI를 보내야
// 한다.
namespace kernel {

class Process;  // 포인터로만 참조(TimerfdState::ownerProcess) - 전체 정의는 process.h

// [신규, 2026-09-29, SP-A7479F83 §6-C 답변("완전히 정리될 때까지는
// 유지하되, 정리될 것임을 플래그로 미리 마킹해 둔다")] `closing`은
// Close(fd)가 세운다 - `DelayedExecutionQueue::cancel(activeToken)`이
// false(이미 실행됐거나 실행 중)를 반환하는 순간에도 `kOnTimerfdFire`
// 가 다른 코어에서 이미 실행 중이거나 실행 직전일 수 있어, Close()가
// 이 경쟁을 무시하고 fd 슬롯을 즉시 회수하면 그 직후 실행되는
// `kOnTimerfdFire`가 이미 회수/재사용된 상태를 건드리는
// use-after-free가 된다(QU-93140484가 지적한 위험). cancel()이
// false를 반환하면 Close()는 fd 슬롯을 그대로 두고, `kOnTimerfdFire`
// 자신이 `closing`을 확인해 마지막 정리자가 된다(그래서 fd 슬롯을
// 되짚어 지울 수 있도록 ownerProcess/ownerFd를 들고 있다 - Create
// 시점에 채워짐).
struct TimerfdState {
    Spinlock lock;
    uint64_t expirationCount = 0;
    bool periodic = false;
    uint64_t intervalTicks = 0;
    uint64_t activeToken = 0;
    bool closing = false;
    WeakPtr<Process> ownerProcess;
    int32_t ownerFd = -1;
    AsyncTaskWaitQueue pendingReaders;
    void destroy() {}
};

constexpr SyscallEndpointId kSyscallEndpointTimerfdCreate = kMakeSyscallEndpointId(6, 6);
constexpr SyscallEndpointId kSyscallEndpointTimerfdSetTime = kMakeSyscallEndpointId(6, 7);

struct TimerfdCreateArgs {
    bool periodic = false;
    // out
    ChannelError error = ChannelError::None;
    int64_t fd = -1;
};

struct TimerfdSetTimeArgs {
    int32_t fd = -1;
    uint64_t initialTicks = 0;
    uint64_t intervalTicks = 0;  // periodic이 아니면 무시
    // out
    ChannelError error = ChannelError::None;
};

class Timerfd {
public:
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_TIMERFD_H
