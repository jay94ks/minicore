#ifndef USERLAND_LIBS_LIBMC_MC_SOCKET_H
#define USERLAND_LIBS_LIBMC_MC_SOCKET_H

#include "libmc/pnp.h"  // mc::ChannelError 재사용
#include "libmc/syscall.h"
#include "libmc/types.h"

// 소켓 계층(SP-231493CB, PN-CC0F4EAC)의 유저랜드 쪽 ABI 거울(mirror) -
// minicore/kernel/socket.h의 SocketDomain/SocketType/SocketArgs/
// SocketBindArgs/SocketListenArgs/SocketAcceptArgs/SocketConnectArgs/
// 엔드포인트 상수와 바이트 단위로 정확히 같은 레이아웃이어야 한다
// (vfs.h/channel.h와 동일한 관례 - 커널 헤더를 유저랜드 freestanding
// 툴체인이 직접 include할 수 없어 손으로 거울 복사). **커널 쪽
// 구조체가 바뀌면 이 파일도 함께 갱신해야 한다.** Send/Recv 전용
// syscall은 없다 - libmc/vfs.h의 기존 Read/Write(그룹3)를 그대로 쓴다.

namespace mc {

enum class SocketDomain : uint32_t {
    Unix = 1,
};

enum class SocketType : uint32_t {
    Stream = 1,
    Datagram = 2,
};

// [신규, 2026-09-27, SP-231493CB §5] 커널 socket.h의 ShutdownHow와
// 바이트 단위로 동일해야 한다.
enum class ShutdownHow : uint32_t {
    Read = 1,
    Write = 2,
    Both = 3,
};

constexpr SyscallEndpointId kSyscallEndpointSocket = kMakeSyscallEndpointId(11, 0);
constexpr SyscallEndpointId kSyscallEndpointSocketBind = kMakeSyscallEndpointId(11, 1);
constexpr SyscallEndpointId kSyscallEndpointSocketListen = kMakeSyscallEndpointId(11, 2);
constexpr SyscallEndpointId kSyscallEndpointSocketAccept = kMakeSyscallEndpointId(11, 3);
constexpr SyscallEndpointId kSyscallEndpointSocketConnect = kMakeSyscallEndpointId(11, 4);
constexpr SyscallEndpointId kSyscallEndpointSocketShutdown = kMakeSyscallEndpointId(11, 5);

struct SocketArgs {
    SocketDomain domain = SocketDomain::Unix;
    SocketType type = SocketType::Stream;
    // out
    ChannelError error = ChannelError::None;
    int64_t fd = -1;
};

struct SocketBindArgs {
    int32_t fd = -1;
    const char* path = nullptr;
    uint32_t pathLen = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct SocketListenArgs {
    int32_t fd = -1;
    uint32_t backlog = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct SocketAcceptArgs {
    int32_t fd = -1;
    // out
    ChannelError error = ChannelError::None;
    int64_t newFd = -1;
};

struct SocketConnectArgs {
    int32_t fd = -1;
    const char* path = nullptr;
    uint32_t pathLen = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct SocketShutdownArgs {
    int32_t fd = -1;
    ShutdownHow how = ShutdownHow::Both;
    // out
    ChannelError error = ChannelError::None;
};

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_SOCKET_H
