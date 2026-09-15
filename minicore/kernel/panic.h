#ifndef MINICORE_KERNEL_PANIC_H
#define MINICORE_KERNEL_PANIC_H

namespace kernel {

// 예외 프레임과 무관한 일반 목적 커널 패닉(SP-EAB162FC §6.4, PN-645CF608
// - "KernelService resurrect limit exceeded" 등) - idt.cpp의 파일 범위
// kPanic(InterruptFrame*)와 달리 트랩 컨텍스트가 없는 평범한 함수 호출
// 지점(예: SelfTerminateHandler::onExec)에서도 부를 수 있도록 메시지
// 문자열 하나만 받는다. 메시지를 로그로 남긴 뒤 영원히 멈추다 - 절대
// 반환하지 않는다.
[[noreturn]] void kPanic(const char* message);

}  // namespace kernel

#endif  // MINICORE_KERNEL_PANIC_H
