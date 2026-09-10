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
// M32(real-libc-syscall-layer.md §M32): 진짜 musl fork()/execve()/
// waitpid()/getpid()로 procsrv의 새 self_register/fork_register
// 오퍼레이션(mc/procsrv_protocol.h)을 왕복시킨다 — musl-hello가
// vfs 외에 procsrv에도 의존해야 한다(servers/CMakeLists.txt
// --depends=musl-hello:vfs,procsrv).
// M35(real-libc-syscall-layer.md §M35, ADR-188): musl locale은 "C"/
// "POSIX" 고정만 검증한다 — 실제 로케일 데이터/iconv/LC_* 전환은
// 범위 밖. setlocale(LC_ALL, "")가 성공하고, 알려지지 않은 로케일
// 이름(예: "ko_KR.UTF-8")을 요청해도 ctype 동작이 여전히 C 로케일
// 그대로임을 확인한다(계획 문서가 원래 기대했던 "알려지지 않은
// 이름은 실패해야 한다"는 실제 musl 동작과 다르다는 것을 소스
// 확인으로 발견 — 아래 main() 안 주석 참고).
// M36(real-libc-syscall-layer.md §M36, ADR-186): 완전한 signal
// 계층. fork()의 자식이 sigaction(SIGUSR1, ...)으로 진짜 musl
// signal 핸들러를 등록하고, 부모가 mc_signal_send()(mc_fork()가
// 함께 내주는 커널 thread 핸들로 — SYS_kill의 pid 라우팅은 procsrv
// 를 거쳐야 해서 이번 라운드는 다루지 않는다, docs/done 참고)로
// SIGUSR1을 보내면 그 핸들러가 실제로 실행됨을 자식의 exit code로
// 확인한다.
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mc/procsrv_client.h>
#include <mc/syscall.h>

// M36 — sigaction()에 등록할 실제 핸들러. 파일 스코프 플래그로
// "실행됐다"는 사실만 남긴다(신호 안전성 상 진짜로 안전한 것은
// 이런 단순 플래그/카운터 갱신 정도뿐이다 — POSIX 관례 그대로).
static volatile int g_sigusr1_count = 0;

static void sigusr1_handler(int sig) {
    (void)sig;
    g_sigusr1_count++;
}

// M37(real-libc-syscall-layer.md §M37, ADR-187) — pthread 최소 구현
// 자기테스트. 두 워커가 같은 address_space를 실제로 공유한다는
// 것(fork()의 COW 분리와 정반대)을 mutex로 보호된 전역 카운터
// 병렬 증가로 확인한다 — mutex 없이 증가했다면 두 스레드가 서로
// 다른 코어에서 실제로 동시에 도는(M34) 이상 값이 일부 증가분을
// 잃어버렸을 것이다.
#define MC_PTHREAD_TEST_INCREMENTS 100000

static pthread_mutex_t g_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static long g_shared_counter = 0;

static void* pthread_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < MC_PTHREAD_TEST_INCREMENTS; i++) {
        pthread_mutex_lock(&g_counter_mutex);
        g_shared_counter++;
        pthread_mutex_unlock(&g_counter_mutex);
    }
    return NULL;
}

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

    // M32(real-libc-syscall-layer.md §M32) — 진짜 musl fork()+
    // execve()+waitpid()로 **다른** 실행 이미지(userland/
    // musl-exec-target, procsrv가 이미 VFS에 심어 둠 —
    // servers/procsrv/main.cpp::run_exec_target_seed())를 실행시키고,
    // 그 자식의 고유한 exit code(42)를 부모가 회수한다. M29의 동적
    // 링킹 재도전은 ADR-203으로 정적 링킹에 되돌아갔으므로, 이
    // execve()는 PT_INTERP 로더 경로를 타지 않는다 — 그것까지
    // 확인하는 것은 이 라운드의 목표가 아니다.
    // procsrv의 g_next_pid는 이미 procsrv 자신의 여러 self-test(M22/
    // M23/M27)가 소비해 둔 뒤라(run_general_process_table_test/
    // run_reparenting_mechanism_test), musl-hello가 받는 실제 pid
    // 값은 그 소비량에 따라 달라진다 — 그래서 정확한 숫자를 기대값
    // 으로 못박지 않고 "0보다 크다"만 확인한다.
    pid_t self_pid = getpid();
    int getpid_ok = (self_pid > 0);
    printf("musl getpid=%d ok=%d\n", (int)self_pid, getpid_ok);
    write_str(getpid_ok ? "musl getpid ok=1\n" : "musl getpid ok=0\n");

    pid_t child = fork();
    if (child == 0) {
        char* const argv[] = {"musl-exec-target.elf", NULL};
        char* const envp[] = {NULL};
        execve("musl-exec-target.elf", argv, envp);
        // execve()가 성공하면 여기로 돌아오지 않는다 — 실패했을 때만.
        write_str("musl execve failed\n");
        _exit(127);
    }

    int fork_ok = (child > 0);
    write_str(fork_ok ? "musl fork ok=1\n" : "musl fork ok=0\n");

    if (fork_ok) {
        int status = 0;
        pid_t waited = waitpid(child, &status, 0);
        int wait_ok =
            (waited == child) && WIFEXITED(status) && WEXITSTATUS(status) == 42;
        write_str(wait_ok ? "musl fork+exec+wait ok=1\n" : "musl fork+exec+wait ok=0\n");
    }

    // M36(real-libc-syscall-layer.md §M36, ADR-186) — 완전한 signal
    // 계층. 자식은 sigaction()으로 진짜 핸들러를 등록한 뒤, 부모가
    // 신호를 보낼 때까지 값싼 syscall(getpid — 매 호출이 커널의
    // return-to-user 확인 지점이다)을 반복해 기다린다. 핸들러가
    // 실행됐는지는 자식 자신의 exit code로 부모에게 알린다(fork()
    // 로 COW 분리된 g_sigusr1_count는 부모가 직접 읽을 수 없다).
    pid_t sig_child = fork();
    if (sig_child == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigusr1_handler;
        sigaction(SIGUSR1, &sa, NULL);
        for (int i = 0; i < 2000000 && g_sigusr1_count == 0; i++) {
            getpid();
        }
        _exit(g_sigusr1_count > 0 ? 55 : 66);
    }
    int sig_fork_ok = (sig_child > 0);
    if (sig_fork_ok) {
        // pid가 아니라 mc_fork()가 함께 내준 커널 thread 핸들로 직접
        // 보낸다 — SYS_kill(pid, sig)의 pid 라우팅은 procsrv가 그
        // pid의 thread_handle을 실제로 알고 있어야 하는데, M32의
        // fork_register 경로로 만들어진 자식은 procsrv가 그 핸들을
        // 모른다(ADR-206이 이미 남긴 한계, kill()도 같은 이유로
        // 지원 안 함) — 그래서 이번 라운드는 mc_signal_send()를 직접
        // 쓴다(docs/done/real-libc-syscall-layer-m36.md 참고).
        //
        // 한 번만 보내지 않고 짧게 재시도한다 — fork() 직후 부모가
        // 자식보다 먼저 스케줄될 수 있어(실제로 QEMU에서 겪음,
        // 2026-09-10), sigaction()으로 핸들러를 등록하기 전에 신호가
        // 도착하면 이 커널의 SIG_DFL은 "무시"로 단순화돼 있어(ADR-186
        // §결정4) 그 신호는 조용히 버려진다. fork()는 COW라 자식이
        // "핸들러 등록 끝났다"는 플래그를 부모가 직접 읽을 수 있는
        // 공유 메모리에 쓸 방법이 없다(위 g_sigusr1_count 주석과 같은
        // 이유) — 그래서 대신 짧은 재전송 루프로 등록이 끝날 때까지의
        // 창을 덮는다. getpid()는 이 프로세스의 pid가 이미
        // procsrv_pid에 캐시돼 있어(M32) 순수 로컬 반환이라 스케줄러를
        // 전혀 건드리지 않는다는 것을 실제로 겪었다(재시도를 아무리
        // 늘려도 자식이 단 한 번도 실행되지 않았다, 2026-09-10) —
        // 대신 진짜로 스케줄러를 양보하는 sched_yield()(M36이 새로
        // 추가, mc/syscall.h::MC_SYSCALL_YIELD)를 각 반복 사이에 쓴다.
        uint32_t child_handle = mc_last_fork_child_thread_handle();
        for (int attempt = 0; attempt < 200; ++attempt) {
            mc_signal_send(child_handle, SIGUSR1);
            sched_yield();
        }

        int status = 0;
        pid_t waited = waitpid(sig_child, &status, 0);
        int sig_ok = (waited == sig_child) && WIFEXITED(status) && WEXITSTATUS(status) == 55;
        write_str(sig_ok ? "musl signal handler ok=1\n" : "musl signal handler ok=0\n");
    }

    // M35(real-libc-syscall-layer.md §M35, ADR-188) — setlocale(LC_ALL,
    // "")는 POSIX 관례상 항상 성공해야 한다(환경변수 기반 로케일
    // 선택 — LC_ALL/LANG 등이 전혀 없는 이 환경에서는 musl 자신의
    // 기본값 "C.UTF-8"로 떨어진다).
    char* loc_empty = setlocale(LC_ALL, "");
    int loc_empty_ok = (loc_empty != NULL);
    write_str(loc_empty_ok ? "musl setlocale empty ok=1\n" : "musl setlocale empty ok=0\n");

    // 계획 문서 원문은 "ko_KR.UTF-8 요청은 실패(NULL)해야 한다"고
    // 적어 뒀지만, third_party/musl/src/locale/locale_map.c::
    // __get_locale()을 실제로 읽어 보면 musl은 **알 수 없는 로케일
    // 이름도 실패시키지 않는다** — 진짜 실패(LOC_MAP_FAILED)는
    // malloc 실패나 이름에 '/'·선행 '.'이 있을 때만 나오고, 그 외엔
    // 항상 "요청한 이름을 기억하되 내부 동작은 C.UTF-8 그대로"로
    // 조용히 대체한다(실제 로케일 아카이브가 없으면 __map_file이
    // 실패해도 그 이름을 담은 __locale_map을 새로 만들어 성공
    // 처리한다). 그래서 이 테스트는 "NULL을 반환해야 한다" 대신
    // ADR-188이 실제로 뜻하는 것("실제 로케일 데이터 없이 C 동작만
    // 유지된다")을 검증한다 — 요청 자체는 성공하지만 ctype 동작은
    // 전혀 안 바뀜을 확인한다.
    char* loc_ko = setlocale(LC_ALL, "ko_KR.UTF-8");
    int loc_ko_ok = (loc_ko != NULL);
    write_str(loc_ko_ok ? "musl setlocale unknown name ok=1\n" : "musl setlocale unknown name ok=0\n");

    int ctype_still_c = (toupper('a') == 'A') && (tolower('B') == 'b');
    write_str(ctype_still_c ? "musl locale ctype still C ok=1\n" : "musl locale ctype still C ok=0\n");

    // M37(real-libc-syscall-layer.md §M37, ADR-187) — 진짜 musl
    // pthread_create()로 워커 둘을 만들고(M34 덕분에 서로 다른
    // 코어에서 동시에 돌 수 있다), pthread_mutex_t로 보호된 공유
    // 카운터를 각각 MC_PTHREAD_TEST_INCREMENTS번 증가시킨 뒤
    // pthread_join()으로 합류해 손실 없이 정확한 합계인지 확인한다.
    pthread_t worker_a, worker_b;
    int create_a = pthread_create(&worker_a, NULL, pthread_worker, NULL);
    int create_b = pthread_create(&worker_b, NULL, pthread_worker, NULL);
    int pthread_create_ok = (create_a == 0) && (create_b == 0);
    write_str(pthread_create_ok ? "musl pthread_create ok=1\n" : "musl pthread_create ok=0\n");

    if (pthread_create_ok) {
        int join_a = pthread_join(worker_a, NULL);
        int join_b = pthread_join(worker_b, NULL);
        int join_ok = (join_a == 0) && (join_b == 0);
        write_str(join_ok ? "musl pthread_join ok=1\n" : "musl pthread_join ok=0\n");

        int counter_ok = (g_shared_counter == 2L * MC_PTHREAD_TEST_INCREMENTS);
        write_str(counter_ok ? "musl pthread mutex counter ok=1\n" : "musl pthread mutex counter ok=0\n");
    }

    fflush(stdout);
    _exit(0);
    return 0;
}
