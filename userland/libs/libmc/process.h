#ifndef USERLAND_LIBS_LIBMC_MC_PROCESS_H
#define USERLAND_LIBS_LIBMC_MC_PROCESS_H

#include "libmc/syscall.h"
#include "libmc/types.h"

// libmc의 Process 관련 syscall 거울 - 커널 쪽
// minicore/kernel/process.h(SP-6BEAE0C1, PN-543C0CE9)와 값/레이아웃을
// 정확히 맞춘다. Wait에 이어 SpawnProcess도 담는다(PN-012D6310 -
// procfs pid 열람 실측 검증의 진짜 선행 공백이었던 "유저랜드에서
// SpawnProcess를 실제로 호출하는 사례"를 위해 추가 - CreateThread 등
// 나머지는 여전히 실제 유저랜드 소비자가 생기면 그때 추가,
// RM-23F4B687 §4).

namespace mc {

// process.h의 kSyscallEndpointWait(그룹0.call3)과 동일한 값.
constexpr SyscallEndpointId kSyscallEndpointWait = kMakeSyscallEndpointId(0, 3);

constexpr int64_t kInvalidProcessId = -1;

// [신규, 2026-09-22, PN-012D6310] process.h의 kSyscallEndpointSpawnProcess
// (그룹0.call4)와 동일한 값.
constexpr SyscallEndpointId kSyscallEndpointSpawnProcess = kMakeSyscallEndpointId(0, 4);

// kernel::SpawnProcessError와 값 순서를 정확히 맞춘다.
enum class SpawnProcessError : unsigned int {
    None = 0,
    InvalidImageRange,
    ImageTooLarge,
    OutOfMemory,
    ElfParseFailed,
    ExecImageFailed,
    InvalidArgument,
    ArgsTooLarge,
};

// kernel::SpawnProcessFlags와 값을 맞춘다.
enum SpawnProcessFlags : unsigned int {
    kSpawnNone = 0,
    kSpawnDebugStart = 1u << 0,
};

// kernel::SpawnProcessArgs와 바이트 단위로 정확히 같은 필드 순서/타입 -
// WaitArgs 문서 주석과 동일한 이유로 순서를 바꾸면 안 된다.
struct SpawnProcessArgs {
    const void* imageBuffer = nullptr;  // in, 유저 포인터 - ELF64 이미지 원본 바이트
    uint64_t imageSize = 0;
    char* const* argv = nullptr;  // in, 유저 포인터, NULL 종단(nullptr이면 빈 argv)
    char* const* envp = nullptr;  // in, 유저 포인터, NULL 종단(nullptr이면 빈 envp)
    uint32_t flags = SpawnProcessFlags::kSpawnNone;
    // out
    SpawnProcessError error = SpawnProcessError::None;
    int64_t pid = kInvalidProcessId;
};

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

// [신규, PN-0556C759] process.h의 kSyscallEndpointCreateThread(그룹0.
// call6)와 동일한 값 - 이 커널 최초의 실제 유저랜드 CreateThread
// 소비자(minicore/dbgtarget, 멀티스레드 하드웨어 브레이크포인트
// 경쟁 재현용)를 위해 추가.
constexpr SyscallEndpointId kSyscallEndpointCreateThread = kMakeSyscallEndpointId(0, 6);

// kernel::CreateThreadError와 값 순서를 정확히 맞춘다.
enum class CreateThreadError : unsigned int {
    None = 0,
    InvalidArgument,
    TooManyThreads,
    OutOfMemory,
};

// kernel::CreateThreadArgs와 바이트 단위로 정확히 같은 필드 순서/타입.
// entry는 SysV 관례대로 RDI=arg 하나만 받는 함수로 취급한다(반환 시
// 동작은 미정의 - 반드시 반환하지 않게 작성).
struct CreateThreadArgs {
    uint64_t entry = 0;      // in, 유저 포인터 - 새 스레드의 시작 함수
    uint64_t arg = 0;        // in - entry(arg) 형태로 RDI에 그대로 전달
    uint64_t stackSize = 0;  // in - 0이면 커널 기본값(64KiB)
    // out
    ThreadId threadId = kInvalidThreadId;
    CreateThreadError error = CreateThreadError::None;
};

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_PROCESS_H
