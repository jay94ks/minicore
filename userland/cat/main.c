// userland/cat/main.c — docs/plan/musl-userland-porting.md §M52
// (ADR-221). 진짜 musl 프로그램 — 각 argv 파일을 열어(open) EOF까지
// 읽어(read) 그대로 표준출력에 쓴다(write). 인자가 없으면 fd 0
// (stdin — 파이프로 dup2된 경우만 실제로 읽을 수 있다, M51)을
// 읽는다. GNU cat의 아주 좁은 부분집합(옵션 없음).
#include <fcntl.h>
#include <unistd.h>

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
    if (argc < 2) {
        return cat_fd(0);
    }
    int status = 0;
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
    return status;
}
