#include "libmc/syscall.h"

namespace mc {

void selfTerminate(int32_t exitCode) {
    // TODO(PN-124C105B): ring3 진입 메커니즘이 트랩 명령(int 0x80 또는
    // syscall)을 확정하면 kSyscallEndpointSelfTerminate로 실제 트랩을
    // 건다. 그 전까지는 인터페이스만 고정해 둔 스텁 - exitCode는 실제
    // 트랩이 생기기 전까지 미사용.
    (void)exitCode;
    for (;;) {
    }
}

}  // namespace mc
