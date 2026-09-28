#ifndef USERLAND_LIBS_LIBMC_MC_TIMERFD_H
#define USERLAND_LIBS_LIBMC_MC_TIMERFD_H

#include "libmc/pnp.h"  // mc::ChannelError
#include "libmc/syscall.h"
#include "libmc/types.h"

// timerfd(SP-A7479F83 §2/§3/§6-A, PN-96265AE4/PN-0F56DE4B)의 유저랜드
// 쪽 ABI 거울 - minicore/kernel/timerfd.h와 바이트 단위로 정확히 같은
// 레이아웃이어야 한다(pnp.h 문서 주석과 동일한 수동 동기화 관례).
// 상대 틱 1회성/주기 타이머 + 절대시각 타이머(§6-A) - RT신호/signalfd는
// 여전히 범위 밖.

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

// [갱신, 2026-09-29, SP-A7479F83 §6-A] absolute=false(기본)면
// initialTicks는 "지금부터 몇 틱 후". true면 initialTicks는 목표
// 유닉스 타임스탬프(초) - 커널이 설정 시점의 Rtc로 한 번만 환산한다.
struct TimerfdSetTimeArgs {
    int32_t fd = -1;
    bool absolute = false;
    uint64_t initialTicks = 0;  // absolute=true면 목표 시각(유닉스 타임스탬프 초)
    uint64_t intervalTicks = 0;  // periodic이 아니면 무시
    // out
    ChannelError error = ChannelError::None;
};

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_TIMERFD_H
