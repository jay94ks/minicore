#ifndef USERLAND_LIBS_LIBMC_MC_VFS_H
#define USERLAND_LIBS_LIBMC_MC_VFS_H

#include "libmc/pnp.h"  // mc::ChannelError 재사용
#include "libmc/syscall.h"
#include "libmc/types.h"

// fs 서비스(minicore/fs, PN-452FF696)가 쓰는 VFS syscall의 유저랜드
// 쪽 ABI 거울(mirror) - minicore/kernel/vfs_syscall.h의
// MountArgs/UnmountArgs/ResolvePathArgs/SignalUserlandReadyArgs/
// WaitForUserlandReadyArgs/엔드포인트 상수와 바이트 단위로 정확히
// 같은 레이아웃이어야 한다(channel.h/pnp.h와 동일한 관례 - 커널
// 헤더를 유저랜드 freestanding 툴체인이 직접 include할 수 없어
// 손으로 거울 복사). **커널 쪽 구조체가 바뀌면 이 파일도 함께
// 갱신해야 한다.**

namespace mc {

// [SP-E9B44929] Vfs 그룹(3) - 커널 쪽과 값을 맞춤(RM-48E1E610).
constexpr SyscallEndpointId kSyscallEndpointMount = kMakeSyscallEndpointId(3, 0);
constexpr SyscallEndpointId kSyscallEndpointUnmount = kMakeSyscallEndpointId(3, 1);
constexpr SyscallEndpointId kSyscallEndpointResolvePath = kMakeSyscallEndpointId(3, 2);
constexpr SyscallEndpointId kSyscallEndpointSignalUserlandReady = kMakeSyscallEndpointId(3, 3);
constexpr SyscallEndpointId kSyscallEndpointWaitForUserlandReady = kMakeSyscallEndpointId(3, 4);

struct MountArgs {
    const char* path = nullptr;
    uint32_t pathLen = 0;
    uint64_t channelId = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct UnmountArgs {
    const char* path = nullptr;
    uint32_t pathLen = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct ResolvePathArgs {
    const char* path = nullptr;
    uint32_t pathLen = 0;
    // out
    ChannelError error = ChannelError::None;
    uint64_t ownerChannelId = 0;
    uint32_t relPathOffset = 0;
};

struct SignalUserlandReadyArgs {
    // out
    ChannelError error = ChannelError::None;
};

struct WaitForUserlandReadyArgs {
    // out
    ChannelError error = ChannelError::None;
};

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_VFS_H
