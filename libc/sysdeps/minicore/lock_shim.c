// libc/sysdeps/minicore/lock_shim.c — musl 내부 락(__lock/__unlock,
// src/internal/lock.h)의 단일 스레드 전용 대체(real-libc-syscall-layer.md
// §M30). 원본 third_party/musl/src/thread/__lock.c는 futex 기반
// congestion 처리까지 구현하는데(pthread는 M37 대상), 이 라운드는
// musl의 lite_malloc.c(§M30)가 요구하는 LOCK/UNLOCK 매크로만 필요할
// 뿐이고 이 커널엔 아직 스레드가 둘 이상 같은 락을 다툴 일이 없다
// (단일 스레드) — __futexwait/__wake 의존성 없이 완전한 no-op으로
// 대체한다.
void __lock(volatile int* l) {
    (void)l;
}

void __unlock(volatile int* l) {
    (void)l;
}

// M31(real-libc-syscall-layer.md §M31) — musl stdio(FLOCK/FUNLOCK,
// src/internal/stdio_impl.h)의 같은 이유 대체. 이 프로젝트가 만드는
// 모든 FILE*(stdout/stderr, __fdopen이 여는 파일)는 항상 `.lock=-1`
// (libc.threaded가 false인 단일 스레드 기본값)이라 FLOCK/FUNLOCK
// 매크로 자신이 이 함수들을 실제로는 절대 호출하지 않는다 — 그래도
// 컴파일된 코드가 심볼을 참조하므로(런타임에 안 타는 분기라도
// 링크는 필요하다) 존재해야 한다. 원본(src/thread/__lockfile.c)은
// __pthread_self/a_cas/__futexwait/a_swap/__wake를 요구하는데,
// 실행되지 않을 코드를 위해 그 의존성을 끌어올 이유가 없다.
#include <stdio.h>

int __lockfile(FILE* f) {
    (void)f;
    return 0;
}

void __unlockfile(FILE* f) {
    (void)f;
}

// M32(real-libc-syscall-layer.md §M32) — fork()가 fork-safety를 위해
// "pthread_create가 진행 중이면 기다린다"는 뜻으로 거는 락
// (__inhibit_ptc/__acquire_ptc/__release_ptc, src/thread/lock_ptc.c)
// 의 같은 이유 대체. 원본은 진짜 pthread_rwlock_wrlock/rdlock/unlock
// 을 쓰는데, 이 프로젝트에는 아직 pthread_create가 없다(M37 대상) —
// 만들어질 스레드가 없으니 다툴 대상도 없다. pthread_rwlock의 futex
// 기반 경쟁 처리 의존성을 이 라운드에 끌어올 이유가 없어 no-op으로
// 대체한다.
void __inhibit_ptc(void) {}

void __acquire_ptc(void) {}

void __release_ptc(void) {}
