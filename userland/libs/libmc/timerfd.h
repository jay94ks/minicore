#ifndef USERLAND_LIBS_LIBMC_MC_TIMERFD_H
#define USERLAND_LIBS_LIBMC_MC_TIMERFD_H

#include "libmc/pnp.h"  // mc::ChannelError
#include "libmc/syscall.h"
#include "libmc/types.h"

// timerfd(SP-A7479F83 §2/§3, PN-96265AE4/PN-0F56DE4B)의 유저랜드 쪽
// ABI 거울 - minicore/kernel/timerfd.h와 바이트 단위로 정확히 같은
// 레이아웃이어야 한다(pnp.h 문서 주석과 동일한 수동 동기화 관례).
// v1 범위는 상대 틱 1회성/주기 타이머뿐 - 절대시각/RT신호/signalfd는
// 범위 밖.

namespace mc {

// RM-48E1E610 그룹6(Event) call6/7과 동일한 값.
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

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_TIMERFD_H
