// libs/mc/include/mc/shell_fd_binding.h — M54(musl-userland-porting.md
// §M54, ADR-225)의 파이프라인(`|`)/출력 리다이렉션(`>`) 지원.
//
// execve()는 새 이미지를 통째로 올린다 — 옛 이미지의 fd 테이블
// (libc/sysdeps/minicore/syscall_shim.c의 g_pipe_fds[]/g_open_files[]
// 는 그 파일 안에서만 아는 로컬 BSS 변수다)이 전혀 남지 않는다.
// dup2()를 exec 전에 불러도(fork()의 경우와 달리) 그 결과가 새
// 이미지에 안 보인다 — 이 프로젝트에 진짜 "fd 진실 공급원"이 아직
// 없다는 것(procsrv.md §3.6, OPEN-64)과 같은 이유다.
//
// 그래서 msh는 파이프/리다이렉션 대상 fd를 execve()의 유일하게
// exec 너머로 살아남는 채널인 **argv**로 직접 실어 보낸다 —
// argv[1..]의 맨 앞에 "@pipefd <fd> <pipe_id>" 또는 "@filefd <fd>
// <fs_handle> <open_file_id>" 토큰을 (필요한 만큼 반복해) 붙이고,
// 대상 프로그램은 자기 main() 맨 앞에서 mc_shell_strip_bindings()
// 하나만 불러 그 토큰들을 실제 바인딩으로 바꾸고 벗겨낸다(진짜
// 인자는 그 뒤에 그대로 남는다). fd는 이번 라운드 목표(파이프
// 사이 연결+표준출력 리다이렉션)에 맞춰 0/1/2만 지원한다.
//
// 파이프 쪽은 M51의 "서버는 fork()/dup2()로 늘어난 참조를 스스로
// 관찰 못 해 호출자가 명시적으로 op_dup을 불러 알려준다"는 계약을
// 그대로 재사용한다(mc_shell_bind_pipe_fd가 그 자리에서 다시
// mc_pipe_dup을 부른다) — 새 서버 프로토콜이 필요 없다.
#pragma once

#include <mc/pipesrv_client.h>
#include <mc/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

// libc/sysdeps/minicore/syscall_shim.c 안에서 정의한다(그 파일의
// 정적 fd 테이블 g_pipe_fds[]/g_std_redirect[]에 직접 접근해야
// 해서). fd 범위 밖이면 조용히 무시한다(msh 자신이 항상 0/1/2만
// 넘기므로 실전에서는 일어나지 않는다). mc_shell_bind_pipe_fd는
// pipe_id 하나를 msh로부터 그대로 "넘겨받는" 것뿐이라(dup 아님 —
// syscall_shim.c의 그 함수 주석 참고) 서버 쪽 참조 카운트를
// 건드리지 않는다.
void mc_shell_bind_pipe_fd(int fd, unsigned long long pipe_id);
void mc_shell_bind_file_fd(int fd, unsigned int fs_handle, unsigned long long open_file_id);

// msh 전용 — fork() 전에 msh 자신의 파이프 fd 항목을 서버에 알리지
// 않고 로컬 표에서만 지운다(syscall_shim.c의 그 함수 주석 참고).
// close()(실제 mc_pipe_close 호출)를 대신 쓰면 그 유일한 참조가
// 자식이 argv로 넘겨받기도 전에 사라진다 — M54가 실제로 겪은 버그.
void mc_shell_forget_pipe_fd(int fd);

// msh가 리다이렉션 대상 파일을 자기 자신이 먼저 연 뒤(open_common()),
// 그 fd가 실제로 어느 {fs_handle, open_file_id}에 묶여 있는지 알아야
// argv에 실어 보낼 수 있다 — g_open_files[]는 syscall_shim.c의 정적
// 표라 msh 쪽에서 직접 못 읽으므로 이 조회 함수가 필요하다. 성공하면
// 1, fd가 열린 VFS 파일이 아니면 0을 반환한다.
int mc_shell_query_file_fd(int fd, unsigned int* out_fs_handle,
                            unsigned long long* out_open_file_id);

// msh 자신이 pipe()로 만든 fd의 실제 pipe_id를 조회한다(파이프라인
// 다음 단계의 argv에 "@pipefd"로 실어 보내려면 필요) — 이유는 위
// mc_shell_query_file_fd와 같다.
int mc_shell_query_pipe_fd(int fd, unsigned long long* out_pipe_id);

// M55(musl-userland-porting.md §M55, ADR-227) — msh가 자기 자식에게
// mc_signal_send()로 이미 시그널을 보낸 뒤 그 사실을 procsrv에게도
// 알린다. syscall_shim.c 안에서 정의한다(그 파일의 MC_PROCSRV_HANDLE
// 관례+mc_getpid_cached()에 직접 접근해야 해서 — msh는 procsrv의
// 핸들 번호를 몰라도 된다, syscall_shim.c의 다른 모든 mc_* 래퍼와
// 같은 원칙). 반환값은 mc/procsrv_protocol.h의 MC_PROC_STATUS_*.
unsigned int mc_shell_report_signaled(unsigned int target_pid, unsigned int signal_number);

// 오버플로 없이 부호 없는 정수를 10진수 문자열로 찍는다(musl의
// snprintf/strtoull을 새로 링크하지 않으려고 직접 짠 최소 구현 —
// msh가 넘기는 값은 전부 fd/handle/id처럼 작은 양수뿐이다).
static inline void mc_shell_u64_to_dec(unsigned long long v, char out[21]) {
    char tmp[20];
    unsigned n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    unsigned i = 0;
    while (n > 0) {
        out[i++] = tmp[--n];
    }
    out[i] = '\0';
}

static inline unsigned long long mc_shell_dec_to_u64(const char* s) {
    unsigned long long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned long long)(*s - '0');
        ++s;
    }
    return v;
}

// argv[1..]의 맨 앞에서 "@pipefd"/"@filefd" 토큰을 발견하는 즉시
// 실제 바인딩을 걸고 그 자리를 지운다 — '@'로 시작하지 않는 첫
// 토큰에서 멈춘다(그 뒤가 진짜 인자다). *pargc를 남은 인자 수로
// 갈아 끼우고 argv[]도 그만큼 앞으로 당긴다(NULL 종료 유지).
static inline void mc_shell_strip_bindings(int* pargc, char* argv[]) {
    int argc = *pargc;
    int src = 1;
    while (src < argc && argv[src][0] == '@') {
        if (argv[src][1] == 'p') {  // "@pipefd" <fd> <pipe_id>
            int fd = (int)mc_shell_dec_to_u64(argv[src + 1]);
            unsigned long long id = mc_shell_dec_to_u64(argv[src + 2]);
            mc_shell_bind_pipe_fd(fd, id);
            src += 3;
        } else {  // "@filefd" <fd> <fs_handle> <open_file_id>
            int fd = (int)mc_shell_dec_to_u64(argv[src + 1]);
            unsigned int h = (unsigned int)mc_shell_dec_to_u64(argv[src + 2]);
            unsigned long long id = mc_shell_dec_to_u64(argv[src + 3]);
            mc_shell_bind_file_fd(fd, h, id);
            src += 4;
        }
    }
    int dst = 1;
    for (int i = src; i < argc; ++i, ++dst) {
        argv[dst] = argv[i];
    }
    argv[dst] = 0;
    *pargc = dst;
}

#ifdef __cplusplus
}  // extern "C"
#endif
