// libk_detail::panic_hook 커널 쪽 정의 (ADR-067) — libk가 "일어나서는
// 안 되는" 상태(cxx-conventions.md §3)에서 호출한다. klog로 로그를
// 남긴 뒤 정지 루프로 멈춘다.
//
// arch 헤더를 include하지 않는다(ADR-002 HAL 경계) — "인터럽트 대기
// 정지" 명령은 모든 지원 아키텍처가 갖고 있어(x86_64 hlt, aarch64
// wfi), 컴파일러가 미리 정의하는 __x86_64__/__aarch64__ 매크로로
// 분기하는 정도는 libk의 cpu_relax()(spinlock.hpp)와 같은 수준의
// 예외로 취급한다.
#include "klog.hpp"

#include <libk/panic.hpp>

namespace libk_detail {

[[noreturn]] void panic_hook(const char* file, int line, const char* msg) {
    klog::printf("[PANIC] %s:%d: %s\n", file, line, msg);

    for (;;) {
#if defined(__x86_64__)
        asm volatile("cli; hlt");
#elif defined(__aarch64__)
        asm volatile("wfi");
#endif
    }
}

}  // namespace libk_detail
