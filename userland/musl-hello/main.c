// userland/musl-hello/main.c — M28의 QEMU 검증 대상(real-libc-syscall-layer.md
// §M28). 기존 셸/서버와 무관한 새 테스트 프로그램 — musl의 **진짜
// 시작 경로**(third_party/musl/crt/crt1.c → __libc_start_main.c →
// 이 main())를 거쳐 진입하고, musl 자신의 write()/_exit() 라이브러리
// 함수(syscall 매크로가 아니라)로 종료한다.
#include <unistd.h>

int main(void) {
    const char msg[] = "hello from real musl\n";
    write(1, msg, sizeof(msg) - 1);
    _exit(0);
    return 0;
}
