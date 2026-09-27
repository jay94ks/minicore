#ifndef USERLAND_LIBS_LIBMC_MC_EPOLL_H
#define USERLAND_LIBS_LIBMC_MC_EPOLL_H

#include "libmc/pnp.h"  // mc::ChannelError 재사용
#include "libmc/syscall.h"
#include "libmc/types.h"

// epoll류 유저 영역 이벤트 다중화(SP-6350DEBB, PN-7562DA62)의 유저랜드
// 쪽 ABI 거울(mirror) - minicore/kernel/epoll.h의 EpollEventMask/
// EpollCtlOp/EpollReadyEvent/EpollCreateArgs/EpollCtlArgs/EpollWaitArgs/
// 엔드포인트 상수와 바이트 단위로 정확히 같은 레이아웃이어야 한다
// (socket.h/vfs.h와 동일한 관례 - 손으로 거울 복사). **커널 쪽 구조체가
// 바뀌면 이 파일도 함께 갱신해야 한다.**

namespace mc {

enum class EpollEventMask : uint32_t {
    Readable = 1u << 0,
    Writable = 1u << 1,
    Error = 1u << 2,
    EdgeTriggered = 1u << 3,  // [보류, v1 미구현] 지정해도 무시(레벨 트리거로 동작)
    OneShot = 1u << 4,        // [보류, v1 미구현] 지정해도 무시
    Exclusive = 1u << 5,      // [보류, v1 미구현] 지정해도 무시
};

enum class EpollCtlOp : uint32_t {
    Add = 1,
    Mod = 2,
    Del = 3,
};

struct EpollReadyEvent {
    uint32_t events = 0;
    uint64_t userData = 0;
};

constexpr SyscallEndpointId kSyscallEndpointEpollCreate = kMakeSyscallEndpointId(6, 3);
constexpr SyscallEndpointId kSyscallEndpointEpollCtl = kMakeSyscallEndpointId(6, 4);
constexpr SyscallEndpointId kSyscallEndpointEpollWait = kMakeSyscallEndpointId(6, 5);

struct EpollCreateArgs {
    // out
    ChannelError error = ChannelError::None;
    int64_t fd = -1;
};

struct EpollCtlArgs {
    int32_t epfd = -1;
    EpollCtlOp op = EpollCtlOp::Add;
    int32_t targetFd = -1;
    uint32_t mask = 0;
    uint64_t userData = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct EpollWaitArgs {
    int32_t epfd = -1;
    EpollReadyEvent* outEvents = nullptr;
    uint32_t maxEvents = 0;
    int64_t timeoutMs = -1;
    // out
    ChannelError error = ChannelError::None;
    int64_t count = 0;
};

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_EPOLL_H
