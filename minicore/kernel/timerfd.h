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
    // [신규, 2026-09-29, SP-6350DEBB §5 통합(PN-0F56DE4B 잔여 범위)]
    // 블로킹 Read 전용인 `pendingReaders`(AsyncTaskWaitQueue, 노드=
    // AsyncTask 자신)와 달리 epoll 관찰자는 channel.h의 소켓
    // readObservers와 동일하게 별도 노드(EpollObserverNode)로 등록해야
    // 한다 - 한 AsyncTask가 EpollWait 하나로 여러 fd를 동시에 감시할 수
    // 있어야 하기 때문(epoll.cpp의 EpollObserverQueue 문서 주석 참고).
    EpollObserverQueue epollReadObservers;
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

// [갱신, 2026-09-29, SP-A7479F83 §6-A(절대시각 타이머)] `absolute`가
// false(기본, 기존 동작)면 `initialTicks`는 "지금부터 몇 틱 후". true면
// `initialTicks`는 `Rtc::toEpochSeconds()`와 같은 형식의 절대
// 유닉스 타임스탬프(초)다 - `TimerfdSetTimeHandler`가 설정 시점에
// `Rtc::readWallClock()`으로 현재 시각을 딱 한 번 읽어 그 차이를
// `kSchedulerTickHz`(scheduler.h, 100Hz)로 환산한 뒤에는 §3의 상대
// 틱 방식과 완전히 동일하게 동작한다(이후 `Rtc` 재조회 없음 - POSIX
// `timerfd_settime(TFD_TIMER_ABSTIME)`가 시스템 시각이 나중에
// 바뀌어도 이미 걸린 타이머의 만료 시각 자체는 재계산하지 않는 것과
// 동일한 단순화, 이 프로젝트는 NTP/시각 재조정 개념이 아직 없어 항상
// 정확하다). 대상 시각이 이미 지났으면 0틱(다음 pump()에서 즉시
// 만료)으로 clamp한다.
struct TimerfdSetTimeArgs {
    int32_t fd = -1;
    bool absolute = false;
    uint64_t initialTicks = 0;  // absolute=true면 목표 시각(유닉스 타임스탬프 초)
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
