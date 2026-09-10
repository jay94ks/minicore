// userland/pipe-test/main.c — 익명 파이프+dup2() 검증(docs/plan/
// musl-userland-porting.md §M51). musl-hello와 같은 이유로 실제
// musl 시작 경로를 그대로 쓴다(linux_abi_stack=1).
//
// 세 가지를 확인한다: (1) 같은 프로세스 안에서 pipe()로 쓰고 쓰기
// 쪽을 닫은 뒤 읽으면 정확한 바이트 수+내용을 받고, 그다음 read()는
// 진짜 EOF(0)를 반환한다. (2) fork()로 pipe의 양쪽 끝을 부모/자식이
// 나눠 가진 뒤(각자 쓰지 않는 쪽을 닫는 표준 관례) 부모가 쓰고
// 자식이 읽는 왕복이 성립한다. (3) dup2()로 파이프의 읽기 쪽을
// fd 0(표준입력)에 덮어씌운 뒤 fd 0을 직접 읽어도 파이프 데이터를
// 받는다.
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void wr(const char* s) {
    write(1, s, strlen(s));
}

int main(void) {
    int all_ok = 1;

    // (1) 단일 프로세스: write -> close(쓰기) -> read -> EOF.
    int fds1[2];
    int ok1 = (pipe(fds1) == 0);
    const char* msg1 = "hello pipe";
    long msg1_len = (long)strlen(msg1);
    if (ok1) {
        long wn = write(fds1[1], msg1, msg1_len);
        ok1 = ok1 && (wn == msg1_len);
        close(fds1[1]);
        char buf[64] = {0};
        long rn = read(fds1[0], buf, sizeof(buf));
        ok1 = ok1 && (rn == msg1_len) && (memcmp(buf, msg1, (size_t)rn) == 0);
        long eofn = read(fds1[0], buf, sizeof(buf));
        ok1 = ok1 && (eofn == 0);
        close(fds1[0]);
    }
    wr(ok1 ? "[pipe-test] single-process write/read/eof ok=1\n"
           : "[pipe-test] single-process write/read/eof ok=0\n");
    all_ok = all_ok && ok1;

    // (2) fork() — 각자 쓰지 않는 쪽을 닫는 표준 관례.
    int fds2[2];
    pipe(fds2);
    const char* msg2 = "fork pipe";
    long msg2_len = (long)strlen(msg2);
    long pid = fork();
    if (pid == 0) {
        close(fds2[1]);
        char cbuf[64] = {0};
        long n = read(fds2[0], cbuf, sizeof(cbuf));
        int child_ok = (n == msg2_len) && (memcmp(cbuf, msg2, (size_t)n) == 0);
        close(fds2[0]);
        _exit(child_ok ? 0 : 1);
    }
    close(fds2[0]);
    write(fds2[1], msg2, msg2_len);
    close(fds2[1]);
    int wstatus = 0;
    long waited = waitpid(pid, &wstatus, 0);
    int ok2 = (waited == pid) && WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
    wr(ok2 ? "[pipe-test] fork pipe roundtrip ok=1\n" : "[pipe-test] fork pipe roundtrip ok=0\n");
    all_ok = all_ok && ok2;

    // (3) dup2() — 파이프 읽기 쪽을 fd 0(stdin)에 덮어씌운다.
    int fds3[2];
    pipe(fds3);
    const char* msg3 = "dup2 data";
    long msg3_len = (long)strlen(msg3);
    write(fds3[1], msg3, msg3_len);
    close(fds3[1]);
    long dup_ret = dup2(fds3[0], 0);
    char dbuf[64] = {0};
    long dn = read(0, dbuf, sizeof(dbuf));
    int ok3 = (dup_ret == 0) && (dn == msg3_len) && (memcmp(dbuf, msg3, (size_t)dn) == 0);
    wr(ok3 ? "[pipe-test] dup2 stdin ok=1\n" : "[pipe-test] dup2 stdin ok=0\n");
    all_ok = all_ok && ok3;
    close(0);
    close(fds3[0]);

    wr(all_ok ? "[pipe-test] all ok=1\n" : "[pipe-test] all ok=0\n");
    return all_ok ? 0 : 1;
}
