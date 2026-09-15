#ifndef USERLAND_LIBS_LIBMC_MC_SYSCALL_H
#define USERLAND_LIBS_LIBMC_MC_SYSCALL_H

#include "libmc/types.h"

// libmc의 syscall 래퍼 - PN-16CA347D 3번(userland/libs/libmc). 커널
// 쪽 SyscallEndpointId 배정(RM-48E1E610)과 값을 맞춰 둔다. 트랩 ABI는
// QU-E7E51931/QU-CD6F68B7로 확정(RAX=verb, RDI=endpointId, RSI=args,
// int 0x80) - syscall.cpp 참고.

namespace mc {

using SyscallEndpointId = uint32_t;

// RM-48E1E610 0번과 같은 값 - Task가 자연 종료할 때(또는 명시적
// selfTerminate() 호출로) 커널에 제출하는 endpoint(kernel::
// kSyscallEndpointSelfTerminate, minicore/kernel/syscall.h 참고).
constexpr SyscallEndpointId kSyscallEndpointSelfTerminate = 0;

// 이 프로세스를 종료한다 - 커널이 트랩 지점에서 이 UserThread를 즉시
// 끝내고 절대 ring3로 복귀시키지 않으므로(PN-71C3D483, QU-D96B1DCE
// 설계자 답변) 실제로 반환하지 않는다.
[[noreturn]] void selfTerminate(int32_t exitCode);

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_SYSCALL_H
