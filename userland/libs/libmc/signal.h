#ifndef USERLAND_LIBS_LIBMC_MC_SIGNAL_H
#define USERLAND_LIBS_LIBMC_MC_SIGNAL_H

#include "libmc/pnp.h"  // mc::ChannelError 재사용(vfs.h와 동일한 관례)
#include "libmc/syscall.h"
#include "libmc/types.h"

// libmc의 Signal 관련 syscall 거울 - 커널 쪽
// minicore/kernel/signal.h(SP-0666DB3C §4.5, RM-B5764185)와 값을
// 정확히 맞춘다. 지금은 SignalAction 하나만 담는다(RM-23F4B687 §4 -
// Kill 등 나머지는 실제 유저랜드 소비자가 생기면 그때 추가) -
// PN-012D6310 실측 검증 중 발견한 실사용 필요(아래 SignalNumber::Chld
// 문서 주석 참고)가 첫 소비자다.

namespace mc {

// signal.h의 kSyscallEndpointSignalAction(그룹0.call2)과 동일한 값.
constexpr SyscallEndpointId kSyscallEndpointSignalAction = kMakeSyscallEndpointId(0, 2);

// kernel::SignalNumber와 값을 맞춘다 - 지금 실사용되는 값만 담는다.
enum class SignalNumber : uint32_t {
    None = 0,
    // [신규, 2026-09-22, PN-012D6310] SIGCHLD - 자식 프로세스 종료
    // 통지. **기본 disposition(Default)이 "프로세스 종료"라서**(POSIX
    // 관례와 달리 이 커널은 Chld를 기본으로 무시하지 않는다,
    // scheduler.cpp의 raiseSignal(Chld) 호출부 문서 주석 참고),
    // SpawnProcess로 자식을 만드는 프로세스는 자식이 죽는 순간
    // 자기 자신도 함께 죽지 않으려면 반드시 이 신호를 명시적으로
    // Ignore로 설정해 둬야 한다 - PN-012D6310 실측 검증 중 이
    // 전제를 몰라서 실제로 init이 처음으로 죽는 것을 실측으로
    // 확인했다.
    Chld = 17,
};

// kernel::SignalDisposition과 값을 맞춘다.
enum class SignalDisposition : uint32_t {
    Default,
    Ignore,
    Handler,
};

// kernel::SignalActionArgs와 바이트 단위로 정확히 같은 필드 순서/타입.
struct SignalActionArgs {
    SignalNumber signal = SignalNumber::None;
    SignalDisposition disposition = SignalDisposition::Default;
    // out
    ChannelError error = ChannelError::None;
};

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_SIGNAL_H
