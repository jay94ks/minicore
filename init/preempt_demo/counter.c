// init/preempt_demo/counter.c — M21(선점형 스케줄링) 검증용 파트너
// 스레드 (docs/plan/general-purpose-completion.md §M21).
//
// busy.c 옆에서 자기 카운터를 증가시키다가 일정 횟수마다 커널 로그로
// 진행 상황을 보고한다 — 이 로그가 실제로 늘어난다는 것 자체가
// "무한루프만 도는 busy 스레드가 옆에 있어도 이 스레드가 계속
// 진행된다"는 타이머 선점의 증거다. libmc를 못 쓰는 이유는 busy.c와
// 같다(이 타깃이 kernel보다 먼저 빌드된다) — 그래서 필요한 두
// syscall(디버그 로그, 스레드 종료)을 이 파일 안에서 직접 감싼다
// (libmc/include/mc/syscall.h와 동일한 ABI, kernel/include/uapi.hpp
// 기준 번호).
#include <stdint.h>

#define K_SYSCALL_THREAD_EXIT 4u
#define K_SYSCALL_DEBUG_LOG 8u
#define MAX_DEBUG_LOG_BYTES 96u

static uint64_t raw_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static void debug_log(const char* msg, uint64_t len) {
    raw_syscall(K_SYSCALL_DEBUG_LOG, (uint64_t)(uintptr_t)msg, len, 0);
}

static _Noreturn void thread_exit(void) {
    for (;;) {
        raw_syscall(K_SYSCALL_THREAD_EXIT, 0, 0, 0);
    }
}

static uint64_t cstr_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

// 아주 작은 uint64 -> 10진 ASCII 변환(libc 없음, init/initrun과 같은
// 이유로 손으로 쓴다).
static uint64_t u64_to_dec(uint64_t v, char* out) {
    char tmp[20];
    uint64_t n = 0;
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (uint64_t i = 0; i < n; ++i) {
        out[i] = tmp[n - 1 - i];
    }
    return n;
}

_Noreturn void _start(const void* arg0) {
    (void)arg0;
    static const char k_prefix[] = "[preempt-demo] counter=";
    const uint64_t k_report_every = 100000ull;  // 보정 없는 값 — QEMU 실측으로 조정(ADR 참고).
    uint64_t reports_left = 20;

    uint64_t counter = 0;
    for (;;) {
        ++counter;
        if (counter % k_report_every != 0) {
            continue;
        }

        char buf[MAX_DEBUG_LOG_BYTES];
        uint64_t plen = cstr_len(k_prefix);
        uint64_t i = 0;
        for (; i < plen; ++i) {
            buf[i] = k_prefix[i];
        }
        char numbuf[20];
        uint64_t nlen = u64_to_dec(counter, numbuf);
        for (uint64_t j = 0; j < nlen && i < MAX_DEBUG_LOG_BYTES; ++j, ++i) {
            buf[i] = numbuf[j];
        }
        if (i < MAX_DEBUG_LOG_BYTES) {
            buf[i++] = '\n';
        }
        debug_log(buf, i);

        if (--reports_left == 0) {
            thread_exit();
        }
    }
}
