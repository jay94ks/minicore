// libk::result/optional/span 등이 오용 시 부르는 panic_hook의 유저랜드
// 구현. 커널(kernel/core/panic.cpp)은 klog+백트레이스+정지를 하지만,
// initrun은 콘솔도 IDT도 없다 — 이 마일스톤에서 이 훅이 실제로 불릴
// 것으로 기대하지 않는다(제대로 만든 코드는 절대 result를 오용하지
// 않는다)만, 링크가 성립하려면 심벌이 있어야 한다. 조용히 멈춘다
// (main.cpp의 quiet_exit()과 같은 이유로 hlt 대신 pause — ring3에서
// hlt는 특권 명령이라 쓸 수 없다).
#include <k/panic.hpp>

namespace libk_detail {

[[noreturn]] void panic_hook(const char* /*file*/, int /*line*/, const char* /*msg*/) {
    for (;;) {
        asm volatile("pause");
    }
}

}  // namespace libk_detail
