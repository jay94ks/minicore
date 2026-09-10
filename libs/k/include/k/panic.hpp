// libk 전역 패닉 훅 (ADR-067). libk 자신은 "복구 불가능한 상태에서 즉시
// 정지"를 어떻게 할지 모른다(커널은 klog+정지 루프, 서버는 exit류
// syscall) — 선언만 하고, 정의는 링크 대상(커널 core 또는 서버 libc
// 계층)이 정확히 하나 제공해야 한다. weak 심볼이 아니다 — 정의 누락은
// 링크 에러로 즉시 드러나야 한다.
#pragma once

namespace libk_detail {

[[noreturn]] void panic_hook(const char* file, int line, const char* msg);

}  // namespace libk_detail

#define LIBK_PANIC(msg) ::libk_detail::panic_hook(__FILE__, __LINE__, (msg))
