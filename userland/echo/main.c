// userland/echo/main.c — docs/plan/musl-userland-porting.md §M52.
// BusyBox 도입을 철회하고 이 저장소 안에서 직접 작성한 최소
// coreutils(ADR-221) 중 하나 — 진짜 musl 프로그램(userland/musl-hello
// 와 같은 이유)이며, `userland/msh`가 fork()+execve()로 별도
// 프로세스로 실행한다(빌트인이 아니다). argv를 그대로 이어 붙여
// 출력한다 — GNU echo의 아주 좁은 부분집합(옵션 없음).
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        write(1, argv[i], strlen(argv[i]));
        if (i + 1 < argc) {
            write(1, " ", 1);
        }
    }
    write(1, "\n", 1);
    return 0;
}
