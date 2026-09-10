// libc/sysdeps/minicore/syscall_shim.c — musl의 모든 syscall이 거치는
// 단일 우회 지점(real-libc-syscall-layer.md §M28, docs/design/
// foundations.md ADR-183 §결정2). third_party/patches/musl/
// 0001-syscall-shim.patch가 patch한 arch/x86_64/syscall_arch.h의
// __syscall0~__syscall6이 전부 여기(__minicore_syscall_dispatch)로
// 온다 — 커널은 이 우회를 전혀 모른다.
//
// ADR-183 §결정4 — 이 파일 자신은 번호별 switch+libmc 호출 한 줄만
// 담는다. 새 IPC/프로토콜 로직을 직접 두지 않는다(SYS_write는 이미
// 있는 mc_debug_log를, SYS_arch_prctl(ARCH_SET_FS)는 이미 있는
// mc_arch_prctl_set_fs를 그대로 호출한다).
//
// 아직 구현하지 않은 syscall 번호를 만나면 항상 -ENOSYS를 반환하고
// 그 사실을 debug_log로 남긴다 — 조용히 무시하거나 잘못된 값을
// 돌려주지 않는다(추후 디버깅을 위해, real-libc-syscall-layer.md
// "검증 방법" 참고).
#include <mc/syscall.h>

#define SYS_write 1
#define SYS_exit 60
#define SYS_arch_prctl 158
#define SYS_exit_group 231

#define ARCH_SET_FS 0x1002

#define MC_EBADF (-9)
#define MC_ENOSYS (-38)

static void log_unimplemented(long n) {
    char buf[64];
    const char* prefix = "[syscall_shim] unimplemented n=";
    unsigned i = 0;
    while (prefix[i] != '\0') {
        buf[i] = prefix[i];
        ++i;
    }
    // n은 음수가 아니다(Linux syscall 번호) — 10진수로 손 변환.
    char digits[20];
    unsigned ndigits = 0;
    unsigned long v = (unsigned long)n;
    if (v == 0) {
        digits[ndigits++] = '0';
    }
    while (v > 0) {
        digits[ndigits++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (ndigits > 0) {
        buf[i++] = digits[--ndigits];
    }
    buf[i++] = '\n';
    mc_debug_log(buf, i);
}

long __minicore_syscall_dispatch(long n, long a, long b, long c, long d, long e, long f) {
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    switch (n) {
        case SYS_write: {
            long fd = a;
            const char* buf = (const char*)(unsigned long)b;
            unsigned long count = (unsigned long)c;
            if (fd != 1 && fd != 2) {
                return MC_EBADF;
            }
            if (count > MC_MAX_DEBUG_LOG_BYTES) {
                count = MC_MAX_DEBUG_LOG_BYTES;
            }
            mc_debug_log(buf, count);
            return (long)count;
        }
        case SYS_exit:
        case SYS_exit_group:
            mc_thread_exit();
            // mc_thread_exit는 _Noreturn이라 여기 도달하지 않는다.
        case SYS_arch_prctl: {
            if (a == ARCH_SET_FS) {
                mc_arch_prctl_set_fs((uint64_t)b);
                return 0;
            }
            return MC_ENOSYS;
        }
        default:
            log_unimplemented(n);
            return MC_ENOSYS;
    }
}
