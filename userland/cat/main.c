// userland/cat/main.c — docs/plan/musl-userland-porting.md §M52
// (ADR-221). 진짜 musl 프로그램 — 각 argv 파일을 열어(open) EOF까지
// 읽어(read) 그대로 표준출력에 쓴다(write). 인자가 없으면 fd 0
// (stdin — 파이프로 dup2된 경우만 실제로 읽을 수 있다, M51)을
// 읽는다. GNU cat의 아주 좁은 부분집합(옵션 없음).
#include <fcntl.h>
#include <unistd.h>

#include <mc/shell_fd_binding.h>

static int cat_fd(int fd) {
    char buf[512];
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            return 1;
        }
        if (n == 0) {
            return 0;
        }
        long off = 0;
        while (off < n) {
            long w = write(1, buf + off, (unsigned long)(n - off));
            if (w <= 0) {
                return 1;
            }
            off += w;
        }
    }
}

int main(int argc, char** argv) {
    // M54(musl-userland-porting.md §M54, ADR-225) — echo/main.c와
    // 같은 이유(msh의 파이프/리다이렉션 argv 관례 벗겨내기).
    mc_shell_strip_bindings(&argc, argv);
    int status;
    if (argc < 2) {
        status = cat_fd(0);
    } else {
        status = 0;
        for (int i = 1; i < argc; ++i) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                status = 1;
                continue;
            }
            if (cat_fd(fd) != 0) {
                status = 1;
            }
            close(fd);
        }
    }
    // M54 — ls/main.c와 같은 이유(다음 파이프라인 단계가 있을 경우
    // fd 0/1의 파이프 참조를 명시적으로 닫아야 한다). 파이프가
    // 아니면(콘솔/이미 닫힌 파일) 조용히 무해한 no-op이다.
    close(0);
    close(1);
    return status;
}
