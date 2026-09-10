// libc/sysdeps/minicore/set_thread_area.c — musl의 __set_thread_area
// (x86_64) 대체(real-libc-syscall-layer.md §M28).
//
// 원본 third_party/musl/src/thread/x86_64/__set_thread_area.s는 raw
// `syscall` 명령을 손으로 직접 발행해 arch_prctl(ARCH_SET_FS, ...)를
// 부른다 — 별도 어셈블리라 third_party/patches/musl/
// 0001-syscall-shim.patch가 패치한 arch/x86_64/syscall_arch.h의
// __syscallN 우회 경로를 전혀 쓰지 않는다(그 헤더 자체를 include하지
// 않는다). 그래서 이 x86_64 전용 .s 파일은 링크 대상에서 뺀다
// (libc/CMakeLists.txt 참고). musl의 범용 대체(src/thread/
// __set_thread_area.c)도 x86_64엔 안 맞는다 — SYS_set_thread_area는
// x86_64에 없는 syscall이라(i386 전용) 무조건 -ENOSYS만 반환한다.
//
// 이 파일이 그 자리를 채운다 — __syscall2 매크로(musl 내부
// src/internal/syscall.h)를 거쳐 반드시 __minicore_syscall_dispatch
// (syscall_shim.c)를 통과하게 만든다(ADR-183 §결정4와 같은 원칙).
#include "syscall.h"

#define ARCH_SET_FS 0x1002

int __set_thread_area(void* p) {
    return __syscall2(SYS_arch_prctl, ARCH_SET_FS, (long)p);
}
