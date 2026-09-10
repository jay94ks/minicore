// userland/msh/main.c — docs/plan/musl-userland-porting.md §M52
// (ADR-221, BusyBox 도입 철회 후 자체 작성). "minicore shell" —
// userland/shell(ADR-170, M20)의 대체가 아니라 그 옆에 두는 새
// 프로그램이다: ADR-170의 셸은 명령을 빌트인으로 직접 처리하고
// fork/exec를 전혀 안 쓴다(당시엔 이 커널에 fork/exec 자체가
// 없었다) — 이 셸은 정확히 반대로, 모든 명령을 별도 실행파일로
// 진짜 fork()+execve()한다(userland/pipe-test가 이미 증명한 M51의
// pipe/dup2와 M32의 fork/execve/waitpid를 그대로 쓴다).
//
// M52는 아직 파이프/리다이렉션을 다루지 않는다(계획 문서 자신이
// M54로 미뤄 뒀다) — 이번 라운드는 "셸이 뜨고, 그 셸이 명령을
// 빌트인이 아니라 별도 실행파일로 fork+exec함"만 증명한다. 키보드
// 입력이 없으면(자동화 환경, servers/login의 관례와 같다) 고정된
// 자기테스트 명령줄 목록을 하나씩 실행한다.
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MC_MAX_TOKENS 8

static void wr(const char* s) {
    write(1, s, strlen(s));
}

// argv를 그대로 이어 붙인다("echo hello msh\n" 같은 로그 한 줄) —
// wr() 여러 번 부르는 것보다 한 번에 로그 한 줄로 남기기 위함.
static void log_running(char* const argv[]) {
    wr("[msh] running:");
    for (int i = 0; argv[i] != 0; ++i) {
        wr(" ");
        wr(argv[i]);
    }
    wr("\n");
}

// line(공백으로 구분된 명령줄)을 그 자리에서 잘라 argv[]를 채운다
// (따옴표/이스케이프 없음 — 계획 문서의 이번 라운드 범위, M54가
// 파이프/리다이렉션을 다룰 때 다시 검토). 반환값은 토큰 수.
static int tokenize(char* line, char* argv[MC_MAX_TOKENS + 1]) {
    int n = 0;
    char* p = line;
    while (*p != '\0' && n < MC_MAX_TOKENS) {
        while (*p == ' ') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        argv[n++] = p;
        while (*p != '\0' && *p != ' ') {
            ++p;
        }
        if (*p == ' ') {
            *p = '\0';
            ++p;
        }
    }
    argv[n] = 0;
    return n;
}

// 명령 이름을 "/bin/<name>" VFS 경로로 매핑한다 — $PATH류 탐색은
// 이 커널에 아직 없다(단순화, YAGNI).
static int run_line(char* line) {
    char* argv[MC_MAX_TOKENS + 1];
    int n = tokenize(line, argv);
    if (n == 0) {
        return 0;
    }
    log_running(argv);

    char path[64];
    path[0] = '\0';
    strcpy(path, "/bin/");
    strcat(path, argv[0]);

    long pid = fork();
    if (pid == 0) {
        execve(path, argv, 0);
        // execve가 성공하면 여기 도달하지 않는다.
        _exit(127);
    }
    int wstatus = 0;
    long waited = waitpid(pid, &wstatus, 0);
    if (waited != pid) {
        wr("[msh] wait error\n");
        return 1;
    }
    if (!WIFEXITED(wstatus)) {
        wr("[msh] child did not exit normally\n");
        return 1;
    }
    return WEXITSTATUS(wstatus);
}

int main(void) {
    wr("[msh] no keyboard input, running self-test commands\n");

    static char cmd1[] = "echo hello msh";
    static char cmd2[] = "ls";
    static char cmd3[] = "cat /bin/echo";
    char* const self_test[] = {cmd1, cmd2, cmd3};

    int all_ok = 1;
    for (unsigned i = 0; i < sizeof(self_test) / sizeof(self_test[0]); ++i) {
        int status = run_line(self_test[i]);
        if (status != 0) {
            all_ok = 0;
        }
    }
    wr(all_ok ? "[msh] self-test done ok=1\n" : "[msh] self-test done ok=0\n");
    return all_ok ? 0 : 1;
}
