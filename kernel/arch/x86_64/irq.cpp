// x86_64 arch_irq_save/arch_irq_restore (ADR-076) — libk::irq_safe<Lock>이
// 쓰는 커널 전용 훅. RFLAGS.IF 비트를 저장하고 로컬 코어 인터럽트를
// 끈 뒤, 나중에 저장된 상태로 복원한다. 서버(유저 프로세스)는 이
// 심벌을 정의하지 않는다 — irq_safe<Lock>은 커널 전용이다.
#include <cstdint>

#include <k/irq_safe.hpp>

namespace libk_detail {

irq_state arch_irq_save() {
    uint64_t flags;
    asm volatile(
        "pushfq\n\t"
        "cli\n\t"
        "pop %0"
        : "=r"(flags)
        :
        : "memory");
    return static_cast<irq_state>(flags);
}

void arch_irq_restore(irq_state state) {
    asm volatile(
        "push %0\n\t"
        "popfq"
        :
        : "r"(static_cast<uint64_t>(state))
        : "memory", "cc");
}

}  // namespace libk_detail
