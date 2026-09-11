// userland/msh/main.c — docs/plan/musl-userland-porting.md §M52
// (ADR-221, BusyBox 도입 철회 후 자체 작성)+§M54(ADR-225, 파이프라인/
// 출력 리다이렉션)+§M55(ADR-226/227, job control 최소)+§M56
// (ADR-228, coreutils를 msh 빌트인으로 흡수). "minicore shell" —
// userland/shell(ADR-170, M20, M53에서 완전히 제거됨)의 대체가
// 아니라 처음부터 그 반대로 설계됐다: 파이프라인/자기테스트 대상
// (loop-test)은 여전히 진짜 fork()+execve()로 별도 프로세스를
// 만든다(M51/M32가 이미 증명한 경로). M56부터는 echo/ls/cat/`[`
// (coreutils)가 더 이상 별도 ELF가 아니라 msh 자신의 함수 호출
// (빌트인)이다 — 파이프라인에서 여러 빌트인이 동시에 진행돼야
// 하므로(예: "ls | cat") msh가 진짜 musl pthread(M37)로 "내부적인
// 병렬 실행"을 흉내낸다. 알려지지 않은 명령은 여전히 "/bin/<name>"
// 을 fork()+execve()하는 기존 경로로 떨어진다(ADR-228 참고 — 셸의
// 일반성은 유지한다). 키보드 입력이 없으면(자동화 환경,
// servers/login의 관례와 같다) 고정된 자기테스트 명령줄 목록을
// 하나씩 실행한다.
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mc/fs_client.h>
#include <mc/pipesrv_client.h>
#include <mc/pipesrv_protocol.h>
#include <mc/procsrv_client.h>
#include <mc/shell_fd_binding.h>
#include <mc/syscall.h>
#include <mc/util.h>
#include <mc/vfs_client.h>

// servers/CMakeLists.txt의 --depends=procsrv:vfs,cfgsrv,pipesrv 순서
// (M53/M54 ADR-224/225)를 procsrv의 start_session_once()가
// inherited_handles로 그대로 msh에게 물려준다 — execve()는 이
// 프로세스의 커널 handle_table 자체를 지우지 않는다(BSS만 새
// 이미지로 갈아엎는다, mc/shell_fd_binding.h 상단 주석과 같은
// 구분) — 그래서 msh는 자기 자신의 handle 2/3/4로 vfs/procsrv/
// pipesrv를 이전에 exec했던 echo/ls/cat이 각자 선언해 두던 것과
// 정확히 같은 값으로 직접 쓸 수 있다(userland/ls의 옛
// #define MC_VFS_HANDLE 2 관례 그대로).
#define MC_VFS_HANDLE 2
#define MC_PIPESRV_HANDLE 4

#define MC_MAX_TOKENS 8
#define MC_MAX_STAGES 4

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
// 줄인다. 없으면 0을 반환한다.
static char* extract_redirect(int* pn, char* argv[]) {
    int n = *pn;
    for (int i = 0; i < n; ++i) {
        if (strcmp(argv[i], ">") == 0 && i + 1 < n) {
            char* target = argv[i + 1];
            argv[i] = 0;
            *pn = i;
            return target;
        }
    }
    return 0;
}

// 명령 이름을 "/bin/<name>" VFS 경로로 매핑한다 — $PATH류 탐색은
// 이 커널에 아직 없다(단순화, YAGNI). 빌트인이 아닌 명령(현재는
// loop-test 하나뿐)의 fork+exec 대상 경로에만 쓰인다.
static void build_path(char* out, unsigned out_size, const char* name) {
    out[0] = '\0';
    strncat(out, "/bin/", out_size - 1);
    strncat(out, name, out_size - strlen(out) - 1);
}

// 파이프라인 한 단계를 fork+exec한다(빌트인이 아닌 명령 전용).
// has_read/read_pipe_id(이전 단계와 이어진 읽기 쪽)와 has_write/
// write_pipe_id(다음 단계와 이어진 쓰기 쪽)는 이미 만들어진 raw
// pipe_id다(run_pipeline이 mc_pipe_create()로 직접 만든다 — M56부터
// msh 자신은 이 id를 자기 fd 테이블(pipe()/dup2())에 전혀 등록하지
// 않는다, 아래 run_pipeline 주석 참고). redirect_target(출력
// 리다이렉션 파일, 없으면 0)은 이 함수가 직접 연다. 셋 다
// mc/shell_fd_binding.h의 "@pipefd"/"@filefd" argv 관례로 자식에게
// 넘긴다(execve()가 자식의 fd 테이블 BSS를 지우므로 dup2()가 아니라
// 이 방식을 쓴다). 반환값은 자식 pid(fork 실패 시 -1).
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

// ---------- M56(ADR-228) — 빌트인 coreutils ----------
//
// 빌트인은 별도 프로세스가 아니라 msh 자신의 pthread(M37)로 실행된다
// (파이프라인의 여러 빌트인이 동시에 진행돼야 하므로 — "내부적으로
// 병렬 실행을 흉내낸다"). pthread는 handle_table을 msh와 그대로
// 공유한다(fork처럼 복제하지 않는다, ADR-212) — 그래서 빌트인은
// msh 자신의 handle 2(vfs)/4(pipesrv)를 그대로 쓸 수 있다.
//
// 하지만 그 공유 때문에, 파이프라인 여러 단계가 "지금 이 스레드의
// fd 1"이라는 하나의 전역 번호(libc/sysdeps/minicore/syscall_shim.c
// 의 g_pipe_fds[]/g_std_redirect[])를 동시에 서로 다른 의미로 쓰려
// 하면 충돌한다(모든 pthread가 같은 handle_table과 같은 프로세스
// BSS를 공유하기 때문). 그래서 빌트인은 그 fd 번호 계층을 완전히
// 우회한다 — 자기 입출력이 파이프인지/파일인지/콘솔인지를 아래
// builtin_input/builtin_output 구조체로 직접 전달받아, pipesrv/vfs
// 클라이언트를 raw id로 직접 부른다. 이 경로엔 M54가 겪은 "fork()의
// 자동 dup" 문제도 없다 — msh가 이 id를 자기 fd 테이블에 등록한
// 적이 없으니 fork()의 그 로직이 볼 것도 없다(빌트인은 fork()도
// 안 한다, pthread_create뿐이다).

typedef struct {
    int is_pipe;
    uint64_t pipe_id;
} builtin_input;

typedef struct {
    int kind;  // 0=콘솔(msh 자신의 진짜 fd 1), 1=파이프, 2=VFS 파일.
    uint64_t pipe_id;
    unsigned int fs_handle;
    uint64_t open_file_id;
} builtin_output;

static long pipe_read_blocking(uint64_t pipe_id, void* buf, unsigned long count) {
    for (;;) {
        uint64_t got = 0;
        uint32_t status = mc_pipe_read(MC_PIPESRV_HANDLE, pipe_id, count, buf, &got);
        if (status == MC_PIPE_STATUS_OK) {
            return (long)got;  // got==0이면 진짜 EOF.
        }
        if (status == MC_PIPE_STATUS_WOULD_BLOCK) {
            mc_yield();
            continue;
        }
        return -1;
    }
}

static long pipe_write_blocking(uint64_t pipe_id, const void* buf, unsigned long count) {
    unsigned long total = 0;
    const uint8_t* p = (const uint8_t*)buf;
    while (total < count) {
        uint64_t written = 0;
        uint32_t status =
            mc_pipe_write(MC_PIPESRV_HANDLE, pipe_id, p + total, count - total, &written);
        if (status == MC_PIPE_STATUS_OK) {
            total += written;
            continue;
        }
        if (status == MC_PIPE_STATUS_WOULD_BLOCK) {
            mc_yield();
            continue;
        }
        break;  // BROKEN_PIPE/그 외 오류 — 더 재시도해도 소용없다.
    }
    return (long)total;
}

static long fs_write_loop(unsigned int fs_handle, uint64_t open_file_id, const void* buf,
                           unsigned long count) {
    unsigned long total = 0;
    const uint8_t* p = (const uint8_t*)buf;
    while (total < count) {
        uint64_t n = mc_fs_write(fs_handle, open_file_id, p + total, count - total);
        if (n == 0) {
            break;
        }
        total += n;
    }
    return (long)total;
}

static long bio_write(builtin_output* out, const void* buf, unsigned long len) {
    if (out->kind == 1) {
        return pipe_write_blocking(out->pipe_id, buf, len);
    }
    if (out->kind == 2) {
        return fs_write_loop(out->fs_handle, out->open_file_id, buf, len);
    }
    return write(1, buf, len);  // 콘솔 — msh 자신의 진짜 fd 1.
}

static long bio_read(builtin_input* in, void* buf, unsigned long cap) {
    if (in->is_pipe) {
        return pipe_read_blocking(in->pipe_id, buf, cap);
    }
    return 0;  // 파이프가 아니면 읽을 stdin이 없다(OPEN-76 — 실제
               // 키보드 입력 경로가 아직 없다).
}

static int builtin_echo(int argc, char** argv, builtin_input* in, builtin_output* out) {
    (void)in;
    for (int i = 1; i < argc; ++i) {
        bio_write(out, argv[i], strlen(argv[i]));
        if (i + 1 < argc) {
            bio_write(out, " ", 1);
        }
    }
    bio_write(out, "\n", 1);
    return 0;
}

static int builtin_ls(int argc, char** argv, builtin_input* in, builtin_output* out) {
    (void)argc;
    (void)argv;
    (void)in;
    // memfs는 평평한 네임스페이스라 OP_LIST에 어느 파일의 fs_handle을
    // 넘기든 결과가 같다 — 아무 파일이나 하나 열어 부트스트랩용으로
    // 쓴다(옛 userland/ls가 "/bin/echo"를 썼던 것과 같은 요령 —
    // echo가 빌트인이 되며 VFS에서 사라져 "/bin/loop-test"(M55,
    // 여전히 별도 ELF)로 바꿨다).
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    if (mc_vfs_open(MC_VFS_HANDLE, "/bin/loop-test", 0, &open_file_id, &fs_handle) !=
        MC_FS_STATUS_OK) {
        bio_write(out, "ls: vfs error\n", 14);
        return 1;
    }
    static uint8_t blob[4096];
    uint32_t count = 0;
    if (mc_fs_list(fs_handle, blob, sizeof(blob), &count) != MC_FS_STATUS_OK) {
        bio_write(out, "ls: list error\n", 15);
        return 1;
    }
    uint64_t off = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const char* name = (const char*)&blob[off];
        uint64_t len = mc_cstr_len(name);
        bio_write(out, name, len);
        bio_write(out, "\n", 1);
        off += len + 1;
    }
    return 0;
}

static int cat_one_file(const char* path, builtin_output* out) {
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    if (mc_vfs_open(MC_VFS_HANDLE, path, 0, &open_file_id, &fs_handle) != MC_FS_STATUS_OK) {
        return 1;
    }
    uint8_t buf[512];
    for (;;) {
        uint64_t n = mc_fs_read(fs_handle, open_file_id, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        bio_write(out, buf, n);
    }
    return 0;
}

static int builtin_cat(int argc, char** argv, builtin_input* in, builtin_output* out) {
    if (argc < 2) {
        uint8_t buf[512];
        for (;;) {
            long n = bio_read(in, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            bio_write(out, buf, (unsigned long)n);
        }
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        if (cat_one_file(argv[i], out) != 0) {
            status = 1;
        }
    }
    return status;
}

// POSIX test/`[`의 아주 좁은 부분집합 — 문자열 비교(=/!=)+정수
// 비교(-eq/-ne/-lt/-le/-gt/-ge)+문자열 비어있음(-z/-n)+단항 문자열
// 진위(비어있지 않으면 참)만 다룬다. `-f`/`-d`(파일 존재 검사)는
// 일부러 뺐다 — mc_vfs_open()이 없는 파일도 그 자리에서 새로 만들어
// 버려(memfs의 open-always-creates 관례, open_common()과 같은
// 근거) "존재하는지"를 부작용 없이 물을 방법이 이 프로토콜에 아직
// 없다. 반환값은 참=0/거짓=1(exit status 관례, msh 자신은 아직
// if/while 같은 조건 분기가 없어 이 값을 소비하지 않지만 셸 관례를
// 미리 맞춰 둔다).
// 부호 있는 10진 정수만 다루는 최소 파서 — musl의 strtol()을 새로
// 링크하지 않으려고 직접 짠다(mc/shell_fd_binding.h의
// mc_shell_dec_to_u64()와 같은 이유).
static long simple_atol(const char* s) {
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        ++s;
    }
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        ++s;
    }
    return neg ? -v : v;
}

static int builtin_test(int argc, char** argv, builtin_input* in, builtin_output* out) {
    (void)in;
    (void)out;
    if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
        return 1;  // "]"로 안 끝나면 형식 오류 — 거짓으로 취급.
    }
    int n = argc - 2;  // "[" 다음부터 "]" 전까지.
    char** a = argv + 1;
    if (n == 1) {
        return a[0][0] != '\0' ? 0 : 1;
    }
    if (n == 2) {
        if (strcmp(a[0], "-z") == 0) {
            return a[1][0] == '\0' ? 0 : 1;
        }
        if (strcmp(a[0], "-n") == 0) {
            return a[1][0] != '\0' ? 0 : 1;
        }
        return 1;
    }
    if (n == 3) {
        const char* op = a[1];
        if (strcmp(op, "=") == 0) {
            return strcmp(a[0], a[2]) == 0 ? 0 : 1;
        }
        if (strcmp(op, "!=") == 0) {
            return strcmp(a[0], a[2]) != 0 ? 0 : 1;
        }
        long lhs = simple_atol(a[0]);
        long rhs = simple_atol(a[2]);
        if (strcmp(op, "-eq") == 0) {
            return lhs == rhs ? 0 : 1;
        }
        if (strcmp(op, "-ne") == 0) {
            return lhs != rhs ? 0 : 1;
        }
        if (strcmp(op, "-lt") == 0) {
            return lhs < rhs ? 0 : 1;
        }
        if (strcmp(op, "-le") == 0) {
            return lhs <= rhs ? 0 : 1;
        }
        if (strcmp(op, "-gt") == 0) {
            return lhs > rhs ? 0 : 1;
        }
        if (strcmp(op, "-ge") == 0) {
            return lhs >= rhs ? 0 : 1;
        }
        return 1;
    }
    return 1;
}

typedef int (*builtin_fn)(int argc, char** argv, builtin_input* in, builtin_output* out);

typedef struct {
    const char* name;
    builtin_fn fn;
} builtin_entry;

static const builtin_entry k_builtins[] = {
    {"echo", builtin_echo},
    {"ls", builtin_ls},
    {"cat", builtin_cat},
    {"[", builtin_test},
};

static builtin_fn lookup_builtin(const char* name) {
    for (unsigned i = 0; i < sizeof(k_builtins) / sizeof(k_builtins[0]); ++i) {
        if (strcmp(k_builtins[i].name, name) == 0) {
            return k_builtins[i].fn;
        }
    }
    return 0;
}

typedef struct {
    builtin_fn fn;
    int argc;
    char** argv;
    builtin_input in;
    builtin_output out;
    int result;
} builtin_thread_ctx;

static void* builtin_thread_main(void* arg) {
    builtin_thread_ctx* ctx = (builtin_thread_ctx*)arg;
    ctx->result = ctx->fn(ctx->argc, ctx->argv, &ctx->in, &ctx->out);
    // 다음 단계가 EOF를 보려면 이 단계가 자기 몫의 파이프 참조를
    // 명시적으로 닫아야 한다(M54가 정립한 "정확히 하나의 참조가
    // 정확히 한 소유자에게" 원칙 그대로 — 이번엔 그 소유자가
    // 프로세스가 아니라 스레드일 뿐이다).
    if (ctx->out.kind == 1) {
        mc_pipe_close(MC_PIPESRV_HANDLE, ctx->out.pipe_id);
    }
    if (ctx->in.is_pipe) {
        mc_pipe_close(MC_PIPESRV_HANDLE, ctx->in.pipe_id);
    }
    return 0;
}

// line(파이프라인 전체, `|`로 나뉜 1개 이상의 단계) 하나를 실행한다.
// 각 단계는 알려진 빌트인이면 pthread로, 아니면(loop-test 등) 기존
// fork+exec 경로(spawn_stage)로 돈다 — 둘을 자유롭게 섞을 수 있다
// (파이프는 어느 쪽이 만들었든 똑같은 raw pipe_id일 뿐이라 무관하다).
// 반환값은 마지막 단계의 종료 코드.
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

    // M56(ADR-228) — 파이프를 msh 자신의 fd 테이블(pipe()/dup2())을
    // 거치지 않고 직접 만든다. 빌트인은 이제 별도 프로세스가 아니라
    // msh **자신의** pthread로 돌아 같은 handle_table/BSS를
    // 공유하므로(ADR-212), fd 번호 하나로 "지금 이 스레드의 fd 1"을
    // 표현하던 종전 관례가 여러 스레드가 동시에 서로 다른 의미로
    // "fd 1"을 쓰려 할 때 충돌한다 — 그래서 raw pipe_id를 직접
    // 다룬다. 이 경로엔 M54가 겪은 "fork()의 자동 dup" 문제도 없다
    // — msh가 이 id를 자기 fd 테이블에 등록한 적이 없으니 그 로직이
    // 볼 것도 없다. 빌트인이 아닌 외부 명령(spawn_stage)에게는 이
    // id를 그대로 argv 토큰으로 넘긴다 — 그쪽은 여전히 M54의
    // "@pipefd" 관례를 그대로 쓴다.
    uint64_t read_ids[MC_MAX_STAGES - 1], write_ids[MC_MAX_STAGES - 1];
    for (int i = 0; i < nstages - 1; ++i) {
        if (!mc_pipe_create(MC_PIPESRV_HANDLE, &read_ids[i], &write_ids[i])) {
            wr("[msh] pipe error\n");
            return 1;
        }
    }

    builtin_fn fn[MC_MAX_STAGES];
    for (int i = 0; i < nstages; ++i) {
        fn[i] = lookup_builtin(stage_argv[i][0]);
    }

    builtin_thread_ctx bctx[MC_MAX_STAGES];
    pthread_t tid[MC_MAX_STAGES];
    long pids[MC_MAX_STAGES];

    for (int i = 0; i < nstages; ++i) {
        int has_read = (i > 0);
        uint64_t read_id = (i > 0) ? read_ids[i - 1] : 0;
        int has_write = (i < nstages - 1);
        uint64_t write_id = (i < nstages - 1) ? write_ids[i] : 0;

        if (fn[i] != 0) {
            log_running(stage_argv[i]);  // 빌트인은 argv 토큰 주입이 없어 그대로 찍는다.
            pids[i] = -1;
            bctx[i].fn = fn[i];
            bctx[i].argc = stage_n[i];
            bctx[i].argv = stage_argv[i];
            bctx[i].in.is_pipe = has_read;
            bctx[i].in.pipe_id = read_id;
            bctx[i].result = 1;
            if (has_write) {
                bctx[i].out.kind = 1;
                bctx[i].out.pipe_id = write_id;
            } else if (redirect_target[i] != 0) {
                uint64_t open_file_id = 0;
                uint32_t fs_handle = 0;
                if (mc_vfs_open(MC_VFS_HANDLE, redirect_target[i], 0, &open_file_id, &fs_handle) ==
                    MC_FS_STATUS_OK) {
                    bctx[i].out.kind = 2;
                    bctx[i].out.fs_handle = fs_handle;
                    bctx[i].out.open_file_id = open_file_id;
                } else {
                    bctx[i].out.kind = 0;  // 리다이렉션 실패 — 콘솔로 새는 게 조용히 사라지는 것보다 낫다.
                }
            } else {
                bctx[i].out.kind = 0;
            }
            pthread_create(&tid[i], 0, builtin_thread_main, &bctx[i]);
        } else {
            pids[i] = spawn_stage(stage_argv[i], has_read, read_id, has_write, write_id,
                                   redirect_target[i]);
        }
    }

    int last_status = 1;
    for (int i = 0; i < nstages; ++i) {
        int status;
        if (fn[i] != 0) {
            pthread_join(tid[i], 0);
            status = bctx[i].result;
        } else {
            int wstatus = 0;
            long waited = waitpid(pids[i], &wstatus, 0);
            status = (waited == pids[i] && WIFEXITED(wstatus)) ? WEXITSTATUS(wstatus) : 1;
        }
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
    // 줄도 실행되기 전에 죽어버린다(실제로 QEMU에서 겪었다). 자식이
    // 실제로 몇 번 스케줄돼 자기 루프에 진입할 시간을 준 뒤에
    // 신호를 보낸다.
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

// M56 — "[ hello = world ]"처럼 거짓이 정답인 자기테스트도 있어,
// 명령마다 "성공(0)이 정답"이 아니라 "이 값이 정답"을 명시한다.
typedef struct {
    char* line;
    int expect_status;
} self_test_entry;

int main(void) {
    wr("[msh] no keyboard input, running self-test commands\n");

    static char cmd1[] = "echo hello msh";
    static char cmd2[] = "ls";
    static char cmd3[] = "cat test.txt";
    static char cmd4[] = "ls | cat";
    static char cmd5[] = "echo msh-redirect-test > /tmp/msh-redirect.txt";
    static char cmd6[] = "cat /tmp/msh-redirect.txt";
    static char cmd7[] = "[ 1 -eq 1 ]";
    static char cmd8[] = "[ hello = world ]";
    self_test_entry self_test[] = {
        {cmd1, 0}, {cmd2, 0}, {cmd3, 0}, {cmd4, 0}, {cmd5, 0}, {cmd6, 0}, {cmd7, 0}, {cmd8, 1},
    };

    int all_ok = 1;
    for (unsigned i = 0; i < sizeof(self_test) / sizeof(self_test[0]); ++i) {
        int status = run_pipeline(self_test[i].line);
        if (status != self_test[i].expect_status) {
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
