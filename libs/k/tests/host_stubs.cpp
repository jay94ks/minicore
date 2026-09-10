// libk가 링크 시점에 요구하는 환경별 훅의 호스트 테스트용 구현
// (ADR-067, ADR-076). 커널/서버 쪽 진짜 구현과 달리, 여기서는 "panic은
// 테스트 실패로 변환"한다(ADR-077 §영향) — 표준 <cstdio>/<cstdlib>를
// 쓴다(이 파일은 freestanding 커널의 일부가 아니라 호스트 네이티브
// 테스트 도구다).
#include <cstdio>
#include <cstdlib>

#include <k/irq_safe.hpp>
#include <k/panic.hpp>

namespace libk_detail {

[[noreturn]] void panic_hook(const char* file, int line, const char* msg) {
    std::fprintf(stderr, "LIBK_PANIC at %s:%d: %s\n", file, line, msg);
    std::abort();
}

// 호스트 테스트는 단일 스레드 - 인터럽트 개념이 없으므로 no-op 스텁이면
// 충분하다(ADR-077 §영향이 이미 이 한계를 인지하고 있다).
irq_state arch_irq_save() { return 0; }
void arch_irq_restore(irq_state) {}

}  // namespace libk_detail
