#include "libmc/syscall.h"

namespace mc {

void selfTerminate(int32_t exitCode) {
    // 실제 트랩 - ABI는 QU-E7E51931/QU-CD6F68B7로 확정(RAX=verb 0=submit,
    // RDI=endpointId, RSI=args, int 0x80). self-terminate는 커널이
    // 트랩 지점에서 이 UserThread를 즉시 끝내고 절대 ring3로 복귀시키지
    // 않는다(QU-D96B1DCE 설계자 답변 - kHandleSyscallTrap이 이 endpoint를
    // 특별 취급) - 그래서 아래 `int $0x80` 다음 줄은 정상적으로는 절대
    // 실행되지 않는다. exitCode는 args로 아직 전달하지 않는다(v1 -
    // 커널 쪽 self-terminate 핸들러가 아직 종료 코드를 안 씀, 필요해지면
    // args 포인터로 확장).
    (void)exitCode;
    const uint64_t endpointId = kSyscallEndpointSelfTerminate;
    asm volatile(
        "mov $0, %%rax\n\t"
        "mov %0, %%rdi\n\t"
        "xor %%esi, %%esi\n\t"
        "int $0x80\n\t"
        :
        : "r"(endpointId)
        : "rax", "rdi", "rsi", "memory");
    // 방어적 - 위 트랩이 정상적으로는 절대 반환하지 않으므로 도달
    // 불가하지만, 컴파일러에게 [[noreturn]] 계약을 지키려면 필요하다.
    for (;;) {
    }
}

}  // namespace mc
