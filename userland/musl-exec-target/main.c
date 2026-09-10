// userland/musl-exec-target/main.c — real-libc-syscall-layer.md §M32.
//
// musl-hello가 fork()로 만든 자식이 execve()로 실행 이미지를 바꿔
// 넣을 **다른** 바이너리(musl-hello 자신이 아니다 — M32의 목표가
// "다른 실행 이미지를 실행"이므로). musl-hello와 마찬가지로 musl의
// 진짜 시작 경로를 거치는 정적 링크 바이너리다(M29의 동적 링킹
// 재도전은 ADR-203으로 이번 계획에서 정적 링킹으로 되돌아갔다 —
// 이 바이너리도 그 기준선을 그대로 따른다. 원래 계획 문서의 "PT_INTERP
// 로더 경로도 함께 확인" 목표는 ADR-203이 이미 접은 부분이라 이
// 라운드의 실제 목표에서 뺐다 — docs/done/real-libc-syscall-layer-m32.md
// 참고).
//
// 표준 출력에 자신이 실행됐다는 증거를 남기고, **부모가 wait4()로
// 회수할 고유한 exit code**로 종료한다 — musl-hello(부모)가 이
// 정확한 값을 받아야 "다른 프로세스가 진짜로 실행되고 그 종료
// 코드가 회수됐다"는 것이 증명된다.
//
// write()/_exit()만 쓴다(printf 대신) — 이 바이너리는 procsrv 자신의
// 컴파일 시점 데이터로 그대로 심겨(tools/bin2c.py) VFS(memfs)에
// 다시 써지는데, memfs 한 파일의 크기 한도(servers/fs/memfs/
// main.cpp::k_max_file_bytes=131072)가 있다. printf 하나만 써도
// vfprintf.c의 전체 서식 처리 체인(부동소수점 포함)이 링크에 끌려와
// musl-hello(스택 전체를 쓰는, 186KiB대) 수준으로 커지고, 그러면
// (a) 이 파일 자체가 128KiB 한도를 넘고 (b) 이 바이트를 그대로 품는
// procsrv 자신의 ELF도 같은 한도를 넘어 procsrv 자신의 기존 M18
// self-exec 왕복 테스트(run_loader_test)까지 실패하게 만든다(실제로
// 겪음, 2026-09-10) — write()/_exit()만 쓰면 musl-hello의 M28
// 최초 버전과 같은 자릿수(수십 KiB)로 충분히 작아진다.
#include <string.h>
#include <unistd.h>

#define K_EXEC_TARGET_EXIT_CODE 42

int main(void) {
    const char* msg = "hello from musl-exec-target (a different image)\n";
    write(1, msg, strlen(msg));
    _exit(K_EXEC_TARGET_EXIT_CODE);
    return 0;
}
