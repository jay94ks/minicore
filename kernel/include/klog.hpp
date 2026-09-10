// klog: 부팅 극초기부터 쓸 수 있는 커널 디버그 콘솔 (docs/spec/debug-console.md).
// arch 독립 선언부 — 실제 init()/putc()는 kernel/arch/${MINICORE_ARCH}가 구현한다
// (ADR-002 HAL 경계: kernel/core는 이 헤더만 include하고 arch 헤더를 모른다).
#pragma once

#include <cstddef>
#include <cstdarg>

namespace kern::klog {

// arch별 UART 초기화. 여러 번 호출해도 안전해야 한다.
void init();

// 1바이트 즉시 송신 (폴링, 블로킹). arch가 구현한다.
void putc(char c);

// putc 반복. kernel/core가 구현하며 arch 헤더를 참조하지 않는다.
void write(const char* s, size_t len);

// 최소 포맷터: %d %u %x %p %s %c %% + l 수식어(%ld %lu %lx, 64비트) 지원
// (debug-console.md §3).
void printf(const char* fmt, ...);
void vprintf(const char* fmt, va_list args);

}  // namespace kern::klog
