#ifndef USERLAND_LIBS_LIBMC_MC_DEBUG_H
#define USERLAND_LIBS_LIBMC_MC_DEBUG_H

#include "libmc/pnp.h"  // mc::ChannelError 재사용
#include "libmc/syscall.h"
#include "libmc/types.h"

// [신규, PN-0556C759] 프로세스 디버깅 syscall(SP-9A6D579F)의 유저랜드
// 쪽 ABI 거울 - minicore/kernel/debug_session.h/syscall.h의
// DebugAttachArgs 등 구조체·엔드포인트 상수와 바이트 단위로 정확히
// 같은 레이아웃이어야 한다(channel.h/pnp.h/vfs.h와 동일한 관례 - 커널
// 헤더를 유저랜드 freestanding 툴체인이 직접 include할 수 없어 손으로
// 거울 복사해 둔다). **커널 쪽 구조체가 바뀌면 이 파일도 함께 갱신해야
// 한다.** 이 커널 최초의 실제 유저랜드 디버깅 syscall 소비자
// (minicore/dbgdriver)를 위해 추가.

namespace mc {

// ThreadId/kInvalidThreadId는 libmc/types.h에 공용으로 정의돼 있다
// (process.h의 CreateThreadArgs도 재사용).

// [SP-E9B44929] Debug 그룹(7) - 커널 쪽과 값을 맞춤(RM-48E1E610).
constexpr SyscallEndpointId kSyscallEndpointDebugAttach = kMakeSyscallEndpointId(7, 0);
constexpr SyscallEndpointId kSyscallEndpointDebugDetach = kMakeSyscallEndpointId(7, 1);
constexpr SyscallEndpointId kSyscallEndpointDebugSetBreakpoint = kMakeSyscallEndpointId(7, 2);
constexpr SyscallEndpointId kSyscallEndpointDebugSetSingleStep = kMakeSyscallEndpointId(7, 3);
constexpr SyscallEndpointId kSyscallEndpointDebugContinue = kMakeSyscallEndpointId(7, 4);
constexpr SyscallEndpointId kSyscallEndpointDebugGetRegisters = kMakeSyscallEndpointId(7, 5);
constexpr SyscallEndpointId kSyscallEndpointDebugSetRegisters = kMakeSyscallEndpointId(7, 6);
constexpr SyscallEndpointId kSyscallEndpointDebugReadMemory = kMakeSyscallEndpointId(7, 7);
constexpr SyscallEndpointId kSyscallEndpointDebugWriteMemory = kMakeSyscallEndpointId(7, 8);

// DR0-DR3 하드웨어 슬롯 수와 동일 - 프로세스당 공유(스레드별 아님).
constexpr uint32_t kMaxDebugBreakpoints = 4;

struct DebugBreakpoint {
    // kernel::DebugBreakpoint::Condition과 정확히 같은 값 - uint8_t
    // 기반(uint32_t 아님, 과거 오프셋 불일치 버그의 재발 방지 - 이
    // 필드를 손으로 옮겨 적을 때 반드시 이 폭을 지킬 것).
    enum class Condition : uint8_t { Execute, Write, ReadWrite };
};

struct DebugAttachArgs {
    int64_t targetProcessId = -1;
    // out
    ChannelError error = ChannelError::None;
};

struct DebugDetachArgs {
    int64_t targetProcessId = -1;
    // out
    ChannelError error = ChannelError::None;
};

// [주의] condition 필드는 DebugBreakpoint::Condition(uint8_t 기반
// enum class)이지 uint32_t가 아니다 - 커널 쪽 구조체와 오프셋이
// 어긋나면 조용히 틀린 동작을 낸다(과거 실제 버그, PN-5E722656).
struct DebugSetBreakpointArgs {
    int64_t targetProcessId = -1;
    uint32_t slot = 0;  // 0..kMaxDebugBreakpoints-1
    uint64_t address = 0;
    DebugBreakpoint::Condition condition = DebugBreakpoint::Condition::Execute;
    bool enable = false;
    // out
    ChannelError error = ChannelError::None;
};

struct DebugSetSingleStepArgs {
    int64_t targetProcessId = -1;
    ThreadId targetThread = kInvalidThreadId;
    bool enable = false;
    // out
    ChannelError error = ChannelError::None;
};

// targetThread가 없다 - DebugContinue는 이 프로세스의 정지된 스레드
// 전부를 한 번에 재개하는 all-stop→continue-all 시맨틱(커널 쪽
// DebugContinueArgs 문서 주석 참고).
struct DebugContinueArgs {
    int64_t targetProcessId = -1;
    // out
    ChannelError error = ChannelError::None;
};

struct DebugReadMemoryArgs {
    int64_t targetProcessId = -1;
    uint64_t address = 0;
    uint64_t length = 0;
    void* out = nullptr;  // 유저 포인터(호출자=디버거 소유 버퍼)
    // out
    ChannelError error = ChannelError::None;
};

struct DebugWriteMemoryArgs {
    int64_t targetProcessId = -1;
    uint64_t address = 0;
    uint64_t length = 0;
    const void* in = nullptr;  // 유저 포인터(호출자=디버거 소유 버퍼)
    // out
    ChannelError error = ChannelError::None;
};

// kernel::DebugRegisterSnapshot(minicore/kernel/syscall.h)과 정확히
// 같은 필드 이름/순서.
struct DebugRegisterSnapshot {
    uint64_t rax = 0, rbx = 0, rcx = 0, rdx = 0, rsi = 0, rdi = 0, rbp = 0;
    uint64_t r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;
    uint64_t rip = 0;
    uint64_t cs = 0;
    uint64_t rflags = 0;
    uint64_t rsp = 0;
    uint64_t ss = 0;
};

struct DebugGetRegistersArgs {
    int64_t targetProcessId = -1;
    ThreadId targetThread = kInvalidThreadId;
    DebugRegisterSnapshot* out = nullptr;  // 유저 포인터(호출자=디버거 소유 버퍼)
    // out
    ChannelError error = ChannelError::None;
};

struct DebugSetRegistersArgs {
    int64_t targetProcessId = -1;
    ThreadId targetThread = kInvalidThreadId;
    const DebugRegisterSnapshot* in = nullptr;  // 유저 포인터(호출자=디버거 소유 버퍼)
    // out
    ChannelError error = ChannelError::None;
};

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_DEBUG_H
