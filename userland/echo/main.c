// userland/echo/main.c — docs/plan/musl-userland-porting.md §M52.
// BusyBox 도입을 철회하고 이 저장소 안에서 직접 작성한 최소
// coreutils(ADR-221) 중 하나 — 진짜 musl 프로그램(userland/musl-hello
// 와 같은 이유)이며, `userland/msh`가 fork()+execve()로 별도
// 프로세스로 실행한다(빌트인이 아니다). argv를 그대로 이어 붙여
// 출력한다 — GNU echo의 아주 좁은 부분집합(옵션 없음).
#include <string.h>
#include <unistd.h>

#include <mc/shell_fd_binding.h>

int main(int argc, char** argv) {
    // M54(musl-userland-porting.md §M54, ADR-225) — msh가 파이프/
    // 리다이렉션 대상으로 넘긴 argv 앞머리("@pipefd"/"@filefd")를
    // 실제 fd 바인딩으로 바꾸고 벗겨낸다. msh를 거치지 않고 그냥
    // 실행됐으면(argv에 그런 토큰이 없으면) 아무 일도 안 한다.
    mc_shell_strip_bindings(&argc, argv);
    for (int i = 1; i < argc; ++i) {
        write(1, argv[i], strlen(argv[i]));
        if (i + 1 < argc) {
            write(1, " ", 1);
        }
    }
    write(1, "\n", 1);
    // M54 — ls/main.c와 같은 이유(파이프/리다이렉션으로 fd 1이 묶여
    // 있으면 명시적으로 닫아야 다음 단계가 EOF를 받는다).
    close(1);
    return 0;
}
