#ifndef USERLAND_LIBS_LIBMC_MC_SYSCALL_H
#define USERLAND_LIBS_LIBMC_MC_SYSCALL_H

#include "libmc/types.h"

// libmc의 syscall 래퍼 - PN-16CA347D 3번(userland/libs/libmc). 커널
// 쪽 SyscallEndpointId 배정(RM-48E1E610)과 값을 맞춰 두되, 실제 트랩
// 명령(int 0x80/syscall)은 PN-124C105B(ring3 진입)가 확정할 때까지
// 정의하지 않는다 - 그 전까지 이 헤더는 "호출 인터페이스만 고정"해
// 둔 스텁이다(syscall.cpp 참고).

namespace mc {

using SyscallEndpointId = uint32_t;

// RM-48E1E610 0번과 같은 값 - Task가 자연 종료할 때 커널에 제출하는
// endpoint(kernel::kSyscallEndpointSelfTerminate, minicore/kernel/
// syscall.h 참고 - 그쪽도 아직 등록된 핸들러가 없음).
constexpr SyscallEndpointId kSyscallEndpointSelfTerminate = 0;

// 프로세스를 종료한다. 실제 트랩 삽입은 PN-124C105B 완료 후 채워진다 -
// 그 전까지는 반환하지 않는 자리표시자(userland/tests/userlandtest의
// _start와 같은 패턴).
[[noreturn]] void selfTerminate(int32_t exitCode);

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_SYSCALL_H
