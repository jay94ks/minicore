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

// [신규, 2026-09-17, SP-E9B44929] 커널 쪽(minicore/kernel/syscall.h)과
// 동일한 Group+Call 2단계 인코딩 - 값을 맞춰 두는 관례 그대로 헬퍼도
// 미러링한다.
constexpr uint32_t kSyscallCallBits = 8;
constexpr uint32_t kSyscallCallMask = 0xFF;

constexpr SyscallEndpointId kMakeSyscallEndpointId(uint8_t group, uint8_t call) {
    return (static_cast<uint32_t>(group) << kSyscallCallBits) | call;
}

// RM-48E1E610 그룹 0(Process), call 0과 같은 값 - Task가 자연 종료할
// 때(또는 명시적 selfTerminate() 호출로) 커널에 제출하는 endpoint
// (kernel::kSyscallEndpointSelfTerminate, minicore/kernel/syscall.h
// 참고). group.call = 0.0이라 우연히 종전 값(0)과 동일.
constexpr SyscallEndpointId kSyscallEndpointSelfTerminate = kMakeSyscallEndpointId(0, 0);

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

// [신규, 2026-09-18, PN-10EE096A] kernel::Syscall::MultiWaitOutcome의
// 유저랜드 거울(mount_table.h류 error enum과 동일한 관례 - 값 순서를
// 정확히 맞춘다: Completed=0, Failed=1, Invalid=2).
enum class MultiWaitOutcome : uint32_t { Completed = 0, Failed = 1, Invalid = 2 };

// [신규, 2026-09-18, PN-10EE096A] kernel::WaitAnyOfSyscallArgs와
// 바이트 단위로 정확히 같은 레이아웃(idt.cpp `kSyscallVerbWaitAnyOf`,
// syscall.h 참고) - verb=2, RDI=이 구조체를 가리키는 포인터.
struct WaitAnyOfSyscallArgs {
    const SyscallToken* tokens = nullptr;  // in
    uint32_t count = 0;                    // in
    // out
    SyscallToken resultToken = 0;
    MultiWaitOutcome resultOutcome = MultiWaitOutcome::Invalid;
};

// tokens 중 아무 하나가 끝날 때까지 블로킹(OR 의미) - 커널 verb=2를
// 그대로 한 번 호출한다. count==0이면 즉시 실패(Invalid) 취급.
WaitAnyOfSyscallArgs waitAnyForMultipleSyscall(const SyscallToken* tokens, uint32_t count);

// tokens로 지정한 N개를 전부 드레인한다(AND 의미) - 커널에는 verb=2
// 하나만 있고(waitAnyForMultipleSyscall과 완전히 같은 구현을 공유,
// QU-31402585/QU-F475C6C2/QU-C06793C2), 이 함수는 유저랜드 쪽에서
// "아직 결과를 못 받은 토큰들"만 추려 그 verb를 최대 count번 반복
// 호출하는 루프를 얹는다(RM-23F4B687 §4 - verb 중복 방지, 새
// 스크래치 버퍼 할당도 방지). **`tokens`는 non-const다** - 이 함수가
// 내부적으로 완료된 항목을 swap-remove하며 그 배열 자체를 작업
// 공간으로 재사용한다(순서가 망가짐, 원본이 필요하면 호출 전에
// 복사해 둔다). 결과는 "완료된 순서"로 outResults[0..count)에
// 채워진다(최소 count칸 필요) - 어떤 토큰이었는지는
// outResults[i].resultToken으로 알 수 있다.
void waitForMultipleSyscall(SyscallToken* tokens, uint32_t count, WaitAnyOfSyscallArgs* outResults);

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_SYSCALL_H
