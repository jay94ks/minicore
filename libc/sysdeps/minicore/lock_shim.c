// libc/sysdeps/minicore/lock_shim.c — musl 내부 락 일부의 단일 스레드
// 전용 대체(real-libc-syscall-layer.md §M30/§M31). __lock/__unlock
// (src/internal/lock.h)은 M37(진짜 pthread, 두 스레드가 같은
// address_space를 공유)부터 이 파일의 no-op이 아니라 musl 원본
// (third_party/musl/src/thread/__lock.c, 진짜 futex 기반 congestion
// 처리)을 그대로 쓴다 — __tl_lock/__tl_unlock(pthread_create.c의
// 스레드 목록 락)이 두 pthread의 pthread_exit()이 거의 동시에
// 끝나는 실제 시나리오에서 정말로 경합할 수 있어(2026-09-10 분석,
// M37 done 참고), no-op으로는 그 목록이 깨질 수 있다는 것을 실제로
// 만들기 전에 미리 알았다.

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
// 을 쓴다. M37이 실제로 pthread_create()를 들여온 뒤에도 이 세
// 함수는 여전히 no-op으로 남긴다 — fork()와 pthread_create()가
// **동시에** 실행되는 시나리오(둘 다 다른 스레드에서 겹쳐 호출)는
// 이 라운드의 자기테스트에 없다(fork()는 M32/M36 테스트가 먼저 끝난
// 뒤에만 쓰고, pthread_create()는 항상 메인 스레드 하나에서만
// 순차적으로 부른다) — 다투는 시나리오 자체가 없으니 진짜
// pthread_rwlock을 새로 끌어올 이유가 없다(__lock/__unlock과 달리,
// 위 주석 참고).
void __inhibit_ptc(void) {}

void __acquire_ptc(void) {}

void __release_ptc(void) {}
