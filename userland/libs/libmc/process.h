#ifndef USERLAND_LIBS_LIBMC_MC_PROCESS_H
#define USERLAND_LIBS_LIBMC_MC_PROCESS_H

#include "libmc/syscall.h"
#include "libmc/types.h"

// libmc의 Process 관련 syscall 거울 - 커널 쪽
// minicore/kernel/process.h(SP-6BEAE0C1, PN-543C0CE9)와 값/레이아웃을
// 정확히 맞춘다. 지금은 init의 자동 회수(reap) 루프가 필요로 하는
// Wait 하나만 담는다(RM-23F4B687 §4 - SpawnProcess/CreateThread 등
// 나머지는 실제 유저랜드 소비자가 생기면 그때 추가).

namespace mc {

// process.h의 kSyscallEndpointWait(그룹0.call3)과 동일한 값.
constexpr SyscallEndpointId kSyscallEndpointWait = kMakeSyscallEndpointId(0, 3);

constexpr int64_t kInvalidProcessId = -1;

// kernel::WaitArgs와 바이트 단위로 정확히 같은 필드 순서/타입 -
// AsyncTaskHandler가 이 구조체를 그대로 static_cast해서 쓰므로 순서를
// 바꾸면 안 된다.
struct WaitArgs {
    int64_t targetPid = kInvalidProcessId;  // in - -1이면 아무 자식이나

    // out - hadZombieChild==true일 때만 reapedPid/exitCode가 유효하다.
    bool hadZombieChild = false;
    int64_t reapedPid = kInvalidProcessId;
    int32_t exitCode = 0;

    // out - targetPid 조건에 맞는 자식이(좀비든 아니든) 하나라도 있었는지.
    bool hasAnyChild = false;
};

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_PROCESS_H
