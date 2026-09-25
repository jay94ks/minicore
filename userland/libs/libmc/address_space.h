#ifndef USERLAND_LIBS_LIBMC_MC_ADDRESS_SPACE_H
#define USERLAND_LIBS_LIBMC_MC_ADDRESS_SPACE_H

#include "libmc/syscall.h"
#include "libmc/types.h"

// libmc의 mmap/munmap/brk syscall 거울 - 커널 쪽
// minicore/kernel/address_space.h(SP-2AAD7C8D §5, RM-48E1E610 17-19번)
// 와 값/레이아웃을 정확히 맞춘다. [신규, 2026-09-26, RM-F2DAFF66
// 실측 발견] 이 세 syscall은 핸들러 자체는 구현돼 있었으나 등록 함수
// (`registerAddressSpaceSyscallEndpoints()`)가 부팅 경로 어디서도
// 불리지 않아 지금까지 완전히 죽어 있었다 - 그 갭을 고치며 이 커널
// 최초의 실제 유저랜드 소비자(이 헤더)를 만든다.

namespace mc {

// address_space.h의 kSyscallEndpointMmap/Munmap/Brk(그룹4 - Memory)와 동일한 값.
constexpr SyscallEndpointId kSyscallEndpointMmap = kMakeSyscallEndpointId(4, 0);
constexpr SyscallEndpointId kSyscallEndpointMunmap = kMakeSyscallEndpointId(4, 1);
constexpr SyscallEndpointId kSyscallEndpointBrk = kMakeSyscallEndpointId(4, 2);

// paging.h의 PAGE_WRITABLE과 동일한 값 - Mmap()의 prot 인자로 그대로
// 넘긴다(PAGE_USER/PAGE_PRESENT는 커널 핸들러가 자동으로 더함).
constexpr uint64_t kPageWritable = 1UL << 1;

// kernel::AddressSpaceError와 값 순서를 정확히 맞춘다.
enum class AddressSpaceError : uint32_t {
    None = 0,
    OutOfMemory,
    InvalidArgument,
    NotMapped,
};

// kernel::MmapArgs와 바이트 단위로 정확히 같은 필드 순서/타입.
struct MmapArgs {
    uint64_t hintAddr = 0;  // v1은 참고만 하고 무시
    uint64_t length = 0;
    uint32_t prot = 0;   // kPageWritable 등 - PAGE_USER는 커널이 자동으로 더함
    uint32_t flags = 0;  // v1 미사용 예약(Anonymous 고정)
    // out
    uint64_t addr = 0;
    AddressSpaceError error = AddressSpaceError::None;
};

// kernel::MunmapArgs와 바이트 단위로 정확히 같은 필드 순서/타입.
struct MunmapArgs {
    uint64_t addr = 0;
    uint64_t length = 0;
    // out
    AddressSpaceError error = AddressSpaceError::None;
};

// kernel::BrkArgs와 바이트 단위로 정확히 같은 필드 순서/타입.
struct BrkArgs {
    uint64_t newBrk = 0;  // 0이면 "현재 brk 조회"
    // out
    uint64_t currentBrk = 0;
    AddressSpaceError error = AddressSpaceError::None;
};

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_ADDRESS_SPACE_H
