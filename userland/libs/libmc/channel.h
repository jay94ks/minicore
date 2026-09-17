#ifndef USERLAND_LIBS_LIBMC_MC_CHANNEL_H
#define USERLAND_LIBS_LIBMC_MC_CHANNEL_H

#include "libmc/pnp.h"  // mc::ChannelError 재사용(그 파일이 이미 이 재사용을 예상해 둠)
#include "libmc/syscall.h"
#include "libmc/types.h"

// Channel IPC(minicore/kernel/channel.h, SP-1FBC0EEB)의 유저랜드 쪽
// ABI 거울 - PN-EAB3A9AE(pubreg 착수 중 발견한 프로젝트 전체 공백).
// pnp.h와 동일한 관례로 커널 헤더를 손으로 거울 복사한다(유저랜드
// freestanding 툴체인이 kernel:: 네임스페이스 코드를 직접 include할
// 수 없음) - **커널 쪽 `channel.h`가 바뀌면 이 파일도 함께 갱신해야
// 한다**(수동 동기화 부담, pnp.h와 동일).
//
// **[정정, 착수 시 확인] pnp.h와 달리 여기엔 얇은 래퍼 함수를 두지
// 않는다** - `minicore/devmgr/main.cpp`(이 프로젝트에 실제로 존재하는
// 유일한 syscall 소비자 선례)가 이미 `mc::submit()`/`mc::wait()`를
// 직접 호출하는 패턴을 확립해 뒀고 `pnp.h` 자신도 그 이상의 래퍼를
// 두지 않았다 - 새 프레임워크/추상화를 추가하지 않고 기존 관례를
// 그대로 따른다(RM-23F4B687 §4).
namespace mc {

using ChannelId = uint64_t;
using BridgeHandle = uint64_t;

// [갱신, 2026-09-17, SP-E9B44929] Channel 그룹(1) - 커널 쪽과 값을 맞춤.
constexpr SyscallEndpointId kSyscallEndpointOpenChannel = kMakeSyscallEndpointId(1, 0);
constexpr SyscallEndpointId kSyscallEndpointConnectChannel = kMakeSyscallEndpointId(1, 1);
constexpr SyscallEndpointId kSyscallEndpointAcceptFromChannel = kMakeSyscallEndpointId(1, 2);
constexpr SyscallEndpointId kSyscallEndpointChannelRead = kMakeSyscallEndpointId(1, 3);
constexpr SyscallEndpointId kSyscallEndpointChannelWrite = kMakeSyscallEndpointId(1, 4);
constexpr SyscallEndpointId kSyscallEndpointCloseBridge = kMakeSyscallEndpointId(1, 5);
constexpr SyscallEndpointId kSyscallEndpointDestroyChannel = kMakeSyscallEndpointId(1, 6);

struct OpenChannelArgs {
    const char* name = nullptr;  // nullptr 또는 nameLength==0 - 이름 없이 개설
    uint64_t nameLength = 0;
    // out
    ChannelError error = ChannelError::None;
    ChannelId channelId = 0;
    BridgeHandle channelHandle = 0;  // v1은 channelId와 같은 값
};

struct ConnectChannelArgs {
    ChannelId target = 0;  // 0이면 name으로 찾는다
    const char* name = nullptr;
    uint64_t nameLength = 0;
    bool useHugePage = false;
    // out
    ChannelError error = ChannelError::None;
    BridgeHandle bridge = 0;
};

struct AcceptFromChannelArgs {
    BridgeHandle channelHandle = 0;
    // out
    ChannelError error = ChannelError::None;
    BridgeHandle bridge = 0;
};

struct ChannelReadArgs {
    BridgeHandle bridge = 0;
    void* buffer = nullptr;
    uint64_t maxLength = 0;
    // out
    ChannelError error = ChannelError::None;
    uint64_t bytesRead = 0;
};

struct ChannelWriteArgs {
    BridgeHandle bridge = 0;
    const void* data = nullptr;
    uint64_t length = 0;
    // out
    ChannelError error = ChannelError::None;
    uint64_t bytesWritten = 0;
};

struct CloseBridgeArgs {
    BridgeHandle bridge = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct DestroyChannelArgs {
    BridgeHandle channelHandle = 0;
    // out
    ChannelError error = ChannelError::None;
};

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_CHANNEL_H
