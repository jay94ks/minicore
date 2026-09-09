// init/preempt_demo/busy.c — M21(선점형 스케줄링) 검증용 유저 스레드
// (docs/plan/general-purpose-completion.md §M21).
//
// 스스로 CPU를 넘겨주는 syscall을 단 한 번도 부르지 않는다(busy-loop) —
// 옆에 있는 counter.c가 진행할 기회를 얻으려면 오직 타이머 선점만이
// 유일한 방법이라는 것을 보이는 것이 이 프로그램의 전체 목적이다.
// **무한루프가 아니다**: 이 프로세스는 이 커널이 실제로 부팅하는
// 모든 경로(smoke-test-x86_64.sh 포함)에 항상 함께 뜨는 데모라, 정말
// 무한히 돌면 이후의 모든 서비스(devmgr/procsrv/shell 등)가 영원히
// CPU 1/N을 이 프로세스에 빼앗겨 부팅 전체가 느려진다(실측: 무한
// busy-loop 버전으로는 smoke-test의 120초 타임아웃 안에 셸 자체
// 테스트까지 못 갔다) — 그래서 데모에 필요한 정도만 반복하고 스스로
// 끝낸다(sys_thread_exit). libmc를 링크하지 않는다(init/initrun과
// 같은 이유 — 이 타깃은 top-level CMakeLists.txt에서 kernel보다
// 먼저 빌드되어 initrd에 심어져야 해서, kernel보다 나중에 추가되는
// libmc를 아직 쓸 수 없다).
#include <stdint.h>

#define K_SYSCALL_THREAD_EXIT 4u

static uint64_t raw_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static _Noreturn void thread_exit(void) {
    for (;;) {
        raw_syscall(K_SYSCALL_THREAD_EXIT, 0, 0, 0);
    }
}

_Noreturn void _start(const void* arg0) {
    (void)arg0;
    // counter.c가 20번 보고(총 2,000,000 증가)를 마치는 것보다 확실히
    // 오래 걸릴 만큼 크게 잡은 값이다(보정 없음, ticks_for()의 M21
    // "보정 없는 값" 정신과 같다) — 실측으로 조정 가능.
    const unsigned long k_iterations = 200000000ul;
    for (unsigned long i = 0; i < k_iterations; ++i) {
        __asm__ volatile("nop");
    }
    thread_exit();
}
