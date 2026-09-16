#include "syscall_fastpath.h"

#include "acpi.h"
#include "gdt.h"
#include "scheduler.h"
#include "syscall.h"
#include "x86_64/msr.h"

namespace {

constexpr kernel::uint32_t kMsrEfer = 0xC0000080;
constexpr kernel::uint64_t kEferSce = 1ULL << 0;
constexpr kernel::uint32_t kMsrStar = 0xC0000081;
constexpr kernel::uint32_t kMsrLstar = 0xC0000082;
constexpr kernel::uint32_t kMsrSfmask = 0xC0000084;
constexpr kernel::uint32_t kMsrGsBase = 0xC0000101;
constexpr kernel::uint32_t kMsrKernelGsBase = 0xC0000102;

// `kSyscallEntry`(syscall_entry.S)가 `swapgs` 직후 gs:[0]/gs:[8]로
// 접근하는 바로 그 구조체 - 오프셋이 어셈블리와 정확히 일치해야
// 한다(필드 추가/순서 변경 시 syscall_entry.S도 반드시 같이 확인).
// 코어마다 하나씩(kAcpiMaxCpus개) - 캐시라인 정렬은 v1에서 신경 쓰지
// 않는다(실측 후 필요하면 조정, RM-23F4B687 §4 원칙).
struct SyscallPerCpuScratch {
    kernel::uint64_t userRspScratch = 0;  // gs:[0] - 진입 시 유저 RSP를 잠깐 보관
    kernel::uint64_t kernelRsp = 0;       // gs:[8] - 이 코어의 현재 커널 RSP(TSS.RSP0 미러)
};

SyscallPerCpuScratch gScratch[kernel::kAcpiMaxCpus];

// [정리, 2026-09-16, PN-F443FE73] 이전엔 이 파일 전용 kReadMsr/
// kWriteMsr가 있었으나, lapic.cpp에도 동일한 중복이 있어 x86_64/
// msr.h(kernel::arch::kReadMsr64/kWriteMsr64)로 통합했다 - 동작
// 변경 없음.
using kernel::arch::kReadMsr64;
using kernel::arch::kWriteMsr64;

}  // namespace

// syscall_entry.S(minicore/libs/x86_64) - `syscall` 명령의 LSTAR
// 목표. `swapgs`로 이 코어의 SyscallPerCpuScratch를 GS_BASE로 끌어와
// gs:[0]에 유저 RSP를 저장하고 gs:[8]의 커널 RSP로 전환한 뒤,
// `kernel::kDispatchSyscallVerb`(syscall.h/idt.cpp)를 호출해 int 0x80
// 경로와 완전히 같은 디스패치 로직을 그대로 재사용한다.
extern "C" void kSyscallEntry();

namespace kernel {

void SyscallFastPath::initForThisCore() {
    const uint32_t coreIndex = Scheduler::currentCoreIndex();

    // KERNEL_GS_BASE = 이 코어 전용 스크래치 주소 - `swapgs`가 현재
    // GS_BASE(유저용)와 이 값을 맞바꾼다. 유저용 GS_BASE는 0으로
    // 시작한다(이 프로젝트는 아직 유저랜드 TLS/GS를 쓰지 않음).
    kWriteMsr64(kMsrKernelGsBase, reinterpret_cast<uint64_t>(&gScratch[coreIndex]));
    kWriteMsr64(kMsrGsBase, 0);

    // STAR[63:48] = SYSRET 베이스(0x10 -> SS=0x18/CS=0x20, gdt.h의
    // kGdtUserDataSelector/kGdtUserCodeSelector 배치와 정확히 대응).
    // STAR[47:32] = SYSCALL 베이스(kGdtKernelCodeSelector=0x08 ->
    // CS=0x08/SS=0x10, boot.S/gdt.cpp의 커널 세그먼트와 대응).
    const uint64_t star =
        (static_cast<uint64_t>(0x10) << 48) | (static_cast<uint64_t>(kGdtKernelCodeSelector) << 32);
    kWriteMsr64(kMsrStar, star);
    kWriteMsr64(kMsrLstar, reinterpret_cast<uint64_t>(&kSyscallEntry));
    // SFMASK - 진입 즉시 클리어할 RFLAGS 비트. IF(9)는 스택 전환이
    // 끝나기 전까지 이 코어에 다른 인터럽트가 끼어들지 못하게 막는
    // 필수 조건(int 0x80의 인터럽트 게이트가 자동으로 하는 일과
    // 동등). TF(8)/DF(10)도 유저가 통제하지 못하게 커널 진입 시
    // 항상 꺼 둔다(표준적인 안전 관례).
    kWriteMsr64(kMsrSfmask, (1ULL << 9) | (1ULL << 8) | (1ULL << 10));

    const uint64_t efer = kReadMsr64(kMsrEfer);
    kWriteMsr64(kMsrEfer, efer | kEferSce);
}

void SyscallFastPath::setKernelRspForThisCore(uint64_t kernelRsp) {
    gScratch[Scheduler::currentCoreIndex()].kernelRsp = kernelRsp;
}

}  // namespace kernel

// syscall_entry.S가 커널 스택으로 전환한 뒤 직접 호출하는 진입점 -
// `kernel::kDispatchSyscallVerb`는 C++ 네임스페이스 안에 있어 이름이
// 맹글링되므로, 어셈블리가 고정된 이름으로 부를 수 있도록 이 얇은
// extern "C" 래퍼를 둔다(다른 곳의 kSyncCr3OnTaskStart 등과 동일한
// 관례 - context_switch.S도 같은 이유로 이런 래퍼를 거친다).
extern "C" kernel::uint64_t kSyscallFastDispatch(kernel::uint64_t verb, kernel::uint64_t arg0,
                                                  kernel::uint64_t arg1) {
    return kernel::kDispatchSyscallVerb(verb, arg0, arg1);
}
