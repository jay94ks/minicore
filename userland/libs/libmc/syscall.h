#ifndef USERLAND_LIBS_LIBMC_MC_SYSCALL_H
#define USERLAND_LIBS_LIBMC_MC_SYSCALL_H

#include "libmc/types.h"

// libmc의 syscall 래퍼 - PN-16CA347D 3번(userland/libs/libmc). 커널
// 쪽 SyscallEndpointId 배정(RM-48E1E610)과 값을 맞춰 둔다. 트랩 ABI는
// QU-E7E51931/QU-CD6F68B7로 확정(RAX=verb, RDI=endpointId, RSI=args,
// int 0x80) - syscall.cpp 참고.

namespace mc {

using SyscallEndpointId = uint32_t;
// [신규, PN-BD9AAE2F devmgr 착수 중 필요해짐] 커널 쪽
// AsyncTaskManageCode(minicore/kernel/async_task.h)와 같은 폭 - submit()
// 이 돌려주는 토큰을 그대로 wait()에 넘기는 용도라 값 자체는 유저랜드
// 입장에서 불투명(opaque)하다.
using SyscallToken = uint64_t;

// RM-48E1E610 0번과 같은 값 - Task가 자연 종료할 때(또는 명시적
// selfTerminate() 호출로) 커널에 제출하는 endpoint(kernel::
// kSyscallEndpointSelfTerminate, minicore/kernel/syscall.h 참고).
constexpr SyscallEndpointId kSyscallEndpointSelfTerminate = 0;

// 이 프로세스를 종료한다 - 커널이 트랩 지점에서 이 UserThread를 즉시
// 끝내고 절대 ring3로 복귀시키지 않으므로(PN-71C3D483, QU-D96B1DCE
// 설계자 답변) 실제로 반환하지 않는다.
[[noreturn]] void selfTerminate(int32_t exitCode);

// [신규, PN-BD9AAE2F devmgr 착수 중 필요해짐 - RM-48E1E610 8번
// EnumerateDevices 등 "제출/대기 분리" 패턴을 쓰는 모든 syscall의
// 공용 진입점] SP-04EE2A18/QU-E7E51931/QU-CD6F68B7가 확정한 ABI
// 그대로(RAX=verb 0, RDI=endpointId, RSI=args) - 블로킹하지 않고
// 즉시 토큰을 반환한다(endpointId가 등록 안 돼 있거나 커널 쪽 자원
// 고갈이면 0, kernel::syscall.h의 "유효하지 않은 토큰" 규약과 동일).
// `args`가 가리키는 메모리는 이 프로세스 자신의 유저 주소공간에
// 속해야 한다(커널이 submitterTask 체이닝으로 검증 - pnp.cpp의
// kValidateEnumerateBuffer류 참고) - 그 구조체 정의 자체는 endpoint별
// 개별 헤더(예: devmgr이 쓸 EnumerateDevices 전용 헤더)의 몫이라
// 여기서는 다루지 않는다.
SyscallToken submit(SyscallEndpointId endpointId, void* args);

// 앞서 submit()이 돌려준 token이 완료될 때까지 블로킹한다(RAX=verb 1,
// RDI=token) - 이미 완료돼 있으면 즉시 반환. 완료(Completed)면 true,
// 실패(Failed)거나 token 자체가 무효(위조/타인 토큰/이미 소비됨)면
// false(kernel::Syscall::wait와 동일한 반환 규약 - 둘을 구분하고
// 싶으면 endpoint별 args 구조체 자신의 error 필드를 본다).
bool wait(SyscallToken token);

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_SYSCALL_H
