// userland/msh/main.c — docs/plan/musl-userland-porting.md §M52
// (ADR-221, BusyBox 도입 철회 후 자체 작성)+§M54(ADR-225, 파이프라인/
// 출력 리다이렉션). "minicore shell" — userland/shell(ADR-170, M20,
// M53에서 완전히 제거됨)의 대체가 아니라 처음부터 그 반대로
// 설계됐다: 모든 명령을 별도 실행파일로 진짜 fork()+execve()한다
// (userland/pipe-test가 이미 증명한 M51의 pipe/dup2와 M32의
// fork/execve/waitpid를 그대로 쓴다).
//
// M54부터 `|`(파이프라인)와 `>`(출력 리다이렉션)를 실제로 지원한다.
// execve()는 fd 테이블을 통째로 지우므로(mc/shell_fd_binding.h 상단
// 주석 참고) dup2()를 exec 전에 걸어도 새 이미지엔 안 남는다 —
// 그래서 파이프/리다이렉션 대상 fd를 argv로 직접 실어 보내고, 대상
// 프로그램(echo/ls/cat)이 자기 main() 맨 앞에서
// mc_shell_strip_bindings()로 그 관례를 벗겨낸다. 키보드 입력이
// 없으면(자동화 환경, servers/login의 관례와 같다) 고정된
// 자기테스트 명령줄 목록을 하나씩 실행한다.
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mc/procsrv_client.h>
#include <mc/shell_fd_binding.h>
#include <mc/syscall.h>

#define MC_MAX_TOKENS 8
#define MC_MAX_STAGES 4

static void wr(const char* s) {
    write(1, s, strlen(s));
}

// argv를 그대로 이어 붙인다("echo hello msh\n" 같은 로그 한 줄) —
// wr() 여러 번 부르는 것보다 한 번에 로그 한 줄로 남기기 위함.
// 파이프라인/리다이렉션 관례 토큰("@pipefd"/"@filefd", spawn_stage
// 참고)이 붙어 있어도 이 함수는 argv를 그대로 다 찍는다 — msh 자신의
// 로그이니 그 배선을 그대로 보여 준다.
static void log_running(char* const argv[]) {
    wr("[msh] running:");
    for (int i = 0; argv[i] != 0; ++i) {
        wr(" ");
        wr(argv[i]);
    }
    wr("\n");
}

// line을 '|'로 나눠 각 세그먼트의 시작 포인터를 stages[]에 채운다
// (그 자리에서 '|'를 '\0'으로 바꿔 끝맺는다 — line을 그대로
// 파괴적으로 잘라 쓴다, 기존 tokenize()와 같은 관례). 반환값은
// 세그먼트 수(파이프가 없으면 1).
static int split_pipeline(char* line, char* stages[MC_MAX_STAGES]) {
    int n = 0;
    char* p = line;
    stages[n++] = p;
    while (*p != '\0' && n < MC_MAX_STAGES) {
        if (*p == '|') {
            *p = '\0';
            ++p;
            stages[n++] = p;
        } else {
            ++p;
        }
    }
    return n;
}

// seg(공백으로 구분된 명령줄 한 조각)을 그 자리에서 잘라 argv[]를
// 채운다(따옴표/이스케이프 없음 — 이 라운드의 범위, 계획 문서가
// 이미 이렇게 좁혀 뒀다). 반환값은 토큰 수.
static int tokenize(char* seg, char* argv[MC_MAX_TOKENS + 1]) {
    int n = 0;
    char* p = seg;
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

// M54 — argv에서 "> 파일" 출력 리다이렉션을 찾아 떼어낸다(마지막
// 파이프라인 단계에만 붙는다고 가정 — 계획 문서의 예시가 그것만
// 요구한다). 찾으면 그 뒤 파일명을 반환하고 *pn을 그 앞까지로
// 줄인다(argv[]의 나머지는 안 지운다 — tokenize가 이미 NUL로
// 끝맺어 뒀으니 *pn 뒤는 그냥 무시된다). 없으면 0을 반환한다.
static char* extract_redirect(int* pn, char* argv[]) {
    int n = *pn;
    for (int i = 0; i < n; ++i) {
        if (strcmp(argv[i], ">") == 0 && i + 1 < n) {
            char* target = argv[i + 1];
            argv[i] = 0;  // spawn_stage는 NUL 종료로 argv_real을 순회한다.
            *pn = i;
            return target;
        }
    }
    return 0;
}

// 명령 이름을 "/bin/<name>" VFS 경로로 매핑한다 — $PATH류 탐색은
// 이 커널에 아직 없다(단순화, YAGNI).
static void build_path(char* out, unsigned out_size, const char* name) {
    out[0] = '\0';
    strncat(out, "/bin/", out_size - 1);
    strncat(out, name, out_size - strlen(out) - 1);
}

// 파이프라인 한 단계를 fork+exec한다. has_read/read_pipe_id(이전
// 단계와 이어진 읽기 쪽)와 has_write/write_pipe_id(다음 단계와
// 이어진 쓰기 쪽)는 msh 자신이 **이미 자기 fd를 닫은 뒤** 미리 뽑아
// 둔 값이다(run_pipeline 참고 — 이유는 그 함수 주석에 있다).
// redirect_target(출력 리다이렉션 파일, 없으면 0)은 이 함수가 직접
// 연다. 셋 다 mc/shell_fd_binding.h의 "@pipefd"/"@filefd" argv
// 관례로 자식에게 넘긴다(execve()가 fd 테이블을 지우므로 dup2()가
// 아니라 이 방식을 쓴다, 그 헤더 상단 주석 참고). 반환값은 자식
// pid(fork 실패 시 -1).
static long spawn_stage(char* argv_real[], int has_read, unsigned long long read_pipe_id,
                         int has_write, unsigned long long write_pipe_id,
                         const char* redirect_target) {
    char path[64];
    build_path(path, sizeof(path), argv_real[0]);

    unsigned int redir_fs_handle = 0;
    unsigned long long redir_open_file_id = 0;
    int has_redirect = 0;
    if (redirect_target != 0) {
        int rfd = open(redirect_target, O_WRONLY | O_CREAT, 0644);
        has_redirect =
            rfd >= 0 && mc_shell_query_file_fd(rfd, &redir_fs_handle, &redir_open_file_id);
    }

    char read_id_buf[21], write_id_buf[21], handle_buf[21], openid_buf[21];
    mc_shell_u64_to_dec(read_pipe_id, read_id_buf);
    mc_shell_u64_to_dec(write_pipe_id, write_id_buf);
    mc_shell_u64_to_dec((unsigned long long)redir_fs_handle, handle_buf);
    mc_shell_u64_to_dec(redir_open_file_id, openid_buf);

    char* extra[10];
    int n_extra = 0;
    if (has_read) {
        extra[n_extra++] = "@pipefd";
        extra[n_extra++] = "0";
        extra[n_extra++] = read_id_buf;
    }
    if (has_write) {
        extra[n_extra++] = "@pipefd";
        extra[n_extra++] = "1";
        extra[n_extra++] = write_id_buf;
    }
    if (has_redirect) {
        extra[n_extra++] = "@filefd";
        extra[n_extra++] = "1";
        extra[n_extra++] = handle_buf;
        extra[n_extra++] = openid_buf;
    }

    char* argv_full[1 + 10 + MC_MAX_TOKENS + 1];
    int ai = 0;
    argv_full[ai++] = argv_real[0];
    for (int i = 0; i < n_extra; ++i) {
        argv_full[ai++] = extra[i];
    }
    for (int i = 1; argv_real[i] != 0; ++i) {
        argv_full[ai++] = argv_real[i];
    }
    argv_full[ai] = 0;

    log_running(argv_full);

    long pid = fork();
    if (pid == 0) {
        execve(path, argv_full, 0);
        // execve가 성공하면 여기 도달하지 않는다.
        _exit(127);
    }
    return pid;
}

// line(파이프라인 전체, `|`로 나뉜 1개 이상의 단계) 하나를 실행한다.
// 파이프가 없으면(nstages==1) M52 시절과 완전히 같은 동작이다 —
// spawn_stage가 read/write_pipe_fd 둘 다 -1이면 "@pipefd" 토큰을
// 하나도 안 붙이므로 argv가 그대로라 로그도 그대로다. 반환값은
// 마지막 단계의 종료 코드.
static int run_pipeline(char* line) {
    char* stage_lines[MC_MAX_STAGES];
    int nstages = split_pipeline(line, stage_lines);

    char* stage_argv[MC_MAX_STAGES][MC_MAX_TOKENS + 1];
    char* redirect_target[MC_MAX_STAGES];
    int stage_n[MC_MAX_STAGES];
    for (int i = 0; i < nstages; ++i) {
        stage_n[i] = tokenize(stage_lines[i], stage_argv[i]);
        redirect_target[i] = (stage_n[i] > 0) ? extract_redirect(&stage_n[i], stage_argv[i]) : 0;
    }
    if (stage_n[0] == 0) {
        return 0;  // 빈 줄.
    }

    int pipe_fds[MC_MAX_STAGES - 1][2];
    unsigned long long read_ids[MC_MAX_STAGES - 1], write_ids[MC_MAX_STAGES - 1];
    int has_read_id[MC_MAX_STAGES - 1], has_write_id[MC_MAX_STAGES - 1];
    for (int i = 0; i < nstages - 1; ++i) {
        if (pipe(pipe_fds[i]) != 0) {
            wr("[msh] pipe error\n");
            return 1;
        }
        has_read_id[i] = mc_shell_query_pipe_fd(pipe_fds[i][0], &read_ids[i]);
        has_write_id[i] = mc_shell_query_pipe_fd(pipe_fds[i][1], &write_ids[i]);
    }

    // M54 실행 중 발견한 진짜 버그 두 가지 — 아래 fork()를 하기
    // **전에** msh 자신의 파이프 fd 표 항목을 지워 둬야 한다.
    // SYS_fork의 자식 쪽 처리(libc/sysdeps/minicore/syscall_shim.c,
    // M51)는 "그 시점에 g_pipe_fds[]에서 살아있는(in_use) 파이프 fd
    // 전부"를 무조건 op_dup으로 참조 카운트에 반영하는데, msh는
    // 파이프 양쪽 끝을 전부 들고 있다가 각 자식에게 argv로 하나씩
    // 넘길 뿐이라 이 자동 dup이 매 fork()마다 불필요하게 참조를
    // 늘렸다(1차 버그, 하다못해 다음 단계가 EOF를 영원히 못 받고
    // mc_yield() 재시도를 무한히 반복하는 행으로 드러났다).
    //
    // 처음엔 이걸 close()로 고쳤는데, close()는 mc_pipe_close로
    // 서버에까지 "이 참조가 끝났다"고 알려 실제로 참조 카운트를
    // 줄인다 — pipe()가 만든 유일한 참조(읽기/쓰기 각 refcount=1)를
    // 자식이 argv로 넘겨받기도 전에 msh가 스스로 지워버려, 서버가
    // 두 refcount 모두 0인 것을 보고 파이프 슬롯을 즉시 반납해
    // 버렸다(2차 버그 — "ls | cat" 자기테스트에서 cat이 조용히
    // status=1로 실패하는 것으로 드러났다: ls의 fd 1이 이미 죽은
    // pipe_id를 mc_pipe_dup하려다 실패해 바인딩이 안 먹혀 콘솔로
    // 새 버렸고, cat의 fd 0도 마찬가지로 안 묶여 read()가 진짜
    // 에러를 냈다). mc_shell_forget_pipe_fd()는 로컬 표만 지워
    // 자동 dup을 막으면서도 서버 쪽 참조는 그대로 살려 둬, 자식이
    // mc_shell_bind_pipe_fd()로 그 유일한 참조를 있는 그대로
    // 넘겨받을 수 있게 한다.
    for (int i = 0; i < nstages - 1; ++i) {
        mc_shell_forget_pipe_fd(pipe_fds[i][0]);
        mc_shell_forget_pipe_fd(pipe_fds[i][1]);
    }

    long pids[MC_MAX_STAGES];
    for (int i = 0; i < nstages; ++i) {
        int has_read = (i > 0) ? has_read_id[i - 1] : 0;
        unsigned long long read_id = (i > 0) ? read_ids[i - 1] : 0;
        int has_write = (i < nstages - 1) ? has_write_id[i] : 0;
        unsigned long long write_id = (i < nstages - 1) ? write_ids[i] : 0;
        pids[i] = spawn_stage(stage_argv[i], has_read, read_id, has_write, write_id,
                               redirect_target[i]);
    }

    int last_status = 1;
    for (int i = 0; i < nstages; ++i) {
        int wstatus = 0;
        long waited = waitpid(pids[i], &wstatus, 0);
        int status = (waited == pids[i] && WIFEXITED(wstatus)) ? WEXITSTATUS(wstatus) : 1;
        if (i == nstages - 1) {
            last_status = status;
        }
    }
    return last_status;
}

// M55(musl-userland-porting.md §M55, ADR-226/227) — job control 최소
// 자기테스트. "프로세스 그룹"의 실제 주체는 procsrv가 아니라 msh
// 자신이다(ADR-227 배경 — procsrv는 fork_register된 자식의 진짜
// 커널 handle을 원천적으로 모른다). `spawn_stage()`를 그대로
// 재사용해 파이프/리다이렉션 없이 `loop-test` 하나만 fork+exec한 뒤,
// 그 직후(spawn_stage()가 그 사이 다른 fork()를 하지 않으므로 여전히
// 유효한) `mc_last_fork_child_thread_handle()`로 자식의 진짜 시그널
// 가능 handle을 얻어 `mc_signal_send(SIGINT)`를 직접 부른다 — 실제
// 종료는 ADR-226의 커널 쪽 기본 동작이 한다. `mc_shell_report_signaled()`
// 로 procsrv에게도 알려야 `waitpid()`가 200,000회 폴링 예산을 다
// 태우지 않고 곧바로 돌아온다(그 op이 exit_code=-SIGINT로 낙관적
// 마킹한다, SYS_wait4의 wstatus 인코딩이 그 값을 하위 8비트로
// 그대로 옮긴다 — WEXITSTATUS가 (unsigned char)(-SIGINT)와 일치하면
// "시그널로 종료됨"으로 본다). 실제 PS/2 키보드 Ctrl-C 감지는 범위
// 밖이라(OPEN-76) 여기서 그 이벤트를 직접 시뮬레이션한다.
static int run_job_control_test(void) {
    char* argv_target[] = {"loop-test", 0};
    long pid = spawn_stage(argv_target, 0, 0, 0, 0, 0);

    // execve()도 여느 syscall과 같은 자리(syscall 리턴 직전, ADR-211
    // §결정3)에서 시그널을 확인한다 — SIGINT를 fork() 직후 곧바로
    // 보내면 자식이 "/bin/loop-test"의 execve() 자체를 마치고
    // 돌아오는 그 순간 바로 소비돼, loop-test의 main()이 단 한
    // 줄도 실행되기 전에 죽어버린다(실제로 QEMU에서 겪었다 — "[msh]
    // job control: child interrupted ok=1"은 나왔지만 "[loop-test]
    // starting"이 로그에 전혀 없었다). 이 자기테스트의 목표는 "실행
    // 중인" 자식을 끊는 것을 보이는 것이라, 자식이 실제로 몇 번
    // 스케줄돼 자기 루프에 진입할 시간을 준 뒤에 신호를 보낸다.
    for (int i = 0; i < 50; ++i) {
        sched_yield();
    }

    unsigned int child_handle = (unsigned int)mc_last_fork_child_thread_handle();
    mc_signal_send(child_handle, SIGINT);
    mc_shell_report_signaled((unsigned int)pid, (unsigned int)SIGINT);

    int wstatus = 0;
    long waited = waitpid(pid, &wstatus, 0);
    int interrupted = (waited == pid) && WIFEXITED(wstatus) &&
                       (WEXITSTATUS(wstatus) == (unsigned char)(-SIGINT));
    wr(interrupted ? "[msh] job control: child interrupted ok=1\n"
                   : "[msh] job control: child interrupted ok=0\n");
    return interrupted ? 0 : 1;
}

int main(void) {
    wr("[msh] no keyboard input, running self-test commands\n");

    static char cmd1[] = "echo hello msh";
    static char cmd2[] = "ls";
    static char cmd3[] = "cat /bin/echo";
    static char cmd4[] = "ls | cat";
    static char cmd5[] = "echo msh-redirect-test > /tmp/msh-redirect.txt";
    static char cmd6[] = "cat /tmp/msh-redirect.txt";
    char* const self_test[] = {cmd1, cmd2, cmd3, cmd4, cmd5, cmd6};

    int all_ok = 1;
    for (unsigned i = 0; i < sizeof(self_test) / sizeof(self_test[0]); ++i) {
        int status = run_pipeline(self_test[i]);
        if (status != 0) {
            all_ok = 0;
        }
    }

    // M55 — 이 단계 뒤에도 self-test 마커가 계속 정상적으로 찍힌다는
    // 사실 자체가 "셸 자신은 살아남는다"는 목표의 증명이다(별도
    // 확인 코드 불필요).
    if (run_job_control_test() != 0) {
        all_ok = 0;
    }

    wr(all_ok ? "[msh] self-test done ok=1\n" : "[msh] self-test done ok=0\n");
    return all_ok ? 0 : 1;
}
