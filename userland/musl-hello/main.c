// userland/musl-hello/main.c — real-libc-syscall-layer.md의
// QEMU 검증 대상. musl의 **진짜 시작 경로**(third_party/musl/crt/
// crt1.c → __libc_start_main.c → 이 main())를 거쳐 진입한다.
//
// M28: write()/_exit()(둘 다 musl 자신의 라이브러리 함수, syscall
// 매크로 직접 호출이 아니다)로 "hello from real musl" 출력.
// M30(real-libc-syscall-layer.md §M30, ADR-183): 진짜 musl
// malloc()(lite_malloc.c, SYS_mmap 기반)으로 버퍼를 할당·채움·
// free()하는 왕복 + errno가 실제로 TLS 경로(M28의 arch_prctl/
// FS_BASE)를 거쳐 읽고 쓰인다는 것을 잘못된 fd로 write()를 호출해
// 확인한다(errno가 EBADF로 설정돼야 한다).
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_str(const char* s) {
    write(1, s, strlen(s));
}

int main(void) {
    write_str("hello from real musl\n");

    // M30 — 진짜 malloc()/free() 왕복.
    char* buf = (char*)malloc(64);
    int malloc_ok = (buf != NULL);
    write_str(malloc_ok ? "musl malloc ok=1\n" : "musl malloc ok=0\n");

    if (malloc_ok) {
        const char payload[] = "musl-hello heap payload";
        memcpy(buf, payload, sizeof(payload));
        int content_ok = (memcmp(buf, payload, sizeof(payload)) == 0);
        write_str(content_ok ? "musl malloc content ok=1\n" : "musl malloc content ok=0\n");
        free(buf);
        write_str("musl free done\n");
    }

    // M30 — errno가 실제로 TLS(스레드별 fs_base) 경로를 거쳐 읽고
    // 쓰이는지 확인한다: 잘못된 fd로 write()를 호출하면
    // syscall_shim.c가 -EBADF를 반환하고, musl의 __syscall_ret이
    // 그것을 errno=EBADF로 변환해야 한다.
    errno = 0;
    long bad_write = write(-1, "x", 1);
    int errno_ok = (bad_write == -1 && errno == EBADF);
    write_str(errno_ok ? "musl errno ok=1\n" : "musl errno ok=0\n");

    _exit(0);
    return 0;
}
