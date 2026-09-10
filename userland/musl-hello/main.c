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
// M31(real-libc-syscall-layer.md §M31): 진짜 musl stdio(fopen/
// fread/fclose, 재구현이 아니라 musl 소스 자체)로 VFS의 실제
// 파일을 열어 읽고, printf로 그 내용을 stdout에 찍어 기대값과
// 일치함을 확인한다. "test.txt"는 procsrv의 기존 자기테스트
// (run_vfs_roundtrip_test, servers/procsrv/main.cpp)가 이미 만들어
// 둔 파일("hello vfs", 9바이트)을 재사용한다 — musl-hello가 vfs
// 하나에만 의존해도(servers/CMakeLists.txt --depends=musl-hello:vfs)
// initrun의 스폰 순서상 procsrv가 이미 그 파일을 써 둔 뒤다.
#include <errno.h>
#include <stdio.h>
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

    // M31 — 진짜 musl stdio(fopen/fread/fclose)로 VFS의 실제 파일을
    // 읽고 printf로 확인한다.
    FILE* f = fopen("test.txt", "r");
    int fopen_ok = (f != NULL);
    write_str(fopen_ok ? "musl fopen ok=1\n" : "musl fopen ok=0\n");

    if (fopen_ok) {
        char content[32] = {0};
        size_t n = fread(content, 1, sizeof(content) - 1, f);
        content[n] = '\0';
        int content_ok = (n == 9 && memcmp(content, "hello vfs", 9) == 0);
        write_str(content_ok ? "musl fread content ok=1\n" : "musl fread content ok=0\n");
        printf("musl printf read: %s (%zu bytes)\n", content, n);
        fclose(f);
    }

    fflush(stdout);
    _exit(0);
    return 0;
}
