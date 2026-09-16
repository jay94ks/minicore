#ifndef USERLAND_LIBS_LIBMC_MC_PNP_H
#define USERLAND_LIBS_LIBMC_MC_PNP_H

#include "libmc/syscall.h"
#include "libmc/types.h"

// devmgr(minicore/devmgr, PN-BD9AAE2F)이 쓰는 PnP 장치 열거 syscall의
// 유저랜드 쪽 ABI 거울(mirror) - minicore/kernel/pnp.h의
// DeviceDescriptor/EnumerateDevicesArgs/kSyscallEndpointEnumerateDevices
// 와 바이트 단위로 정확히 같은 레이아웃이어야 한다(커널 헤더를
// 유저랜드 freestanding 툴체인이 직접 include할 수 없어 - kernel::
// 네임스페이스 코드 전체가 이 툴체인과 호환 안 됨 - 손으로 거울
// 복사해 둔다). **커널 쪽 구조체가 바뀌면 이 파일도 함께 갱신해야
// 한다** - 아직 두 빌드 트리가 공유하는 ABI 헤더 인프라가 없어 생기는
// 수동 동기화 부담(관계도에 기록해 둠).

namespace mc {

struct DeviceDescriptor {
    uint32_t bus = 0, device = 0, function = 0;
    uint32_t vendorId = 0, deviceId = 0;
    uint32_t classCode = 0, subclass = 0, progIf = 0;
    uint64_t mmioBases[6] = {};
    uint32_t irqVector = 0;
};

// kernel::ChannelError(minicore/kernel/channel.h)와 정확히 같은 값 -
// EnumerateDevices는 이 중 InvalidPointer만 실제로 돌려주지만, 나중에
// Channel IPC 유저랜드 래퍼를 추가할 때 그대로 재사용할 수 있도록
// enum 전체를 거울 복사해 둔다.
enum class ChannelError : uint32_t {
    None = 0,
    NameInUse,
    NotFound,
    InvalidHandle,
    ResourceExhausted,
    HugePageUnsupported,
    BrokenPipe,
    InvalidPointer,
};

struct EnumerateDevicesArgs {
    uint32_t startIndex = 0;
    uint32_t capacity = 0;
    DeviceDescriptor* outDevices = nullptr;
    uint32_t totalCount = 0;
    ChannelError error = ChannelError::None;
};

constexpr SyscallEndpointId kSyscallEndpointEnumerateDevices = 8;

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_PNP_H
