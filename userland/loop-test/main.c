// userland/loop-test/main.c — docs/plan/musl-userland-porting.md
// §M55(ADR-227)의 job control 자기테스트 대상. 시작 즉시
// "starting"을 찍고 sched_yield()를 아주 큰 횟수 반복한다 — 아무
// syscall도 안 하는 순수 계산 루프였다면 SIGINT가 전달될 기회
// (ADR-211 §결정3, syscall 리턴 시점 한정)를 영원히 못 잡는다.
// 전부 마치고 나서야 "finished without interruption"을 찍는다 —
// 이 줄이 QEMU 로그에 나타나면 msh의 SIGINT 전달(ADR-226/227)이
// 실패했다는 뜻이다(정상적인 성공 경로에선 핸들러 없는 SIGINT의
// 기본 동작이 이 스레드를 그 자리에서 종료시켜, 이 줄까지 도달할
// 기회 자체가 없다).
#include <sched.h>
#include <unistd.h>

int main(void) {
    write(1, "[loop-test] starting\n", 21);

    for (long i = 0; i < 5000000; ++i) {
        sched_yield();
    }

    write(1, "[loop-test] finished without interruption\n", 42);
    return 0;
}
