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
