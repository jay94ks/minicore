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
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_brk 12
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
        // M30(real-libc-syscall-layer.md §M30) — musl의 lite_malloc.c가
        // 항상 SYS_brk를 먼저 시도한 뒤 실패하면 mmap으로 우회한다
        // (musl 소스 그대로, 원본 로직). 이 SYS_brk를 **항상 실패로
        // 답한다**(호출자가 요청한 값과 다른 값, 여기서는 항상 0을
        // 반환 — Linux의 진짜 brk() 절대주소 관례를 흉내 내려는 게
        // 아니다) — libmc의 mc_malloc이 이미 sys_brk(ADR-180, 힙
        // 슬롯 5)를 직접 쓰고 있어, musl의 malloc도 같은 커널 상태를
        // 공유하면 서로의 캐시된 커서가 어긋나 겹칠 위험이 있다
        // (kernel_objects.hpp::address_space::mmap_top 주석 참고) —
        // 이렇게 항상 실패시키면 musl의 malloc은 무조건 SYS_mmap
        // 경로(완전히 분리된 새 영역)로만 가게 되어 그 위험이
        // 원천적으로 없어진다.
        case SYS_brk:
            return 0;
        case SYS_mmap: {
            // a=addr(무시, 항상 NULL 취급), b=length, c=prot(무시),
            // d=flags(무시 — 이 커널은 익명 매핑만 지원), e=fd(무시),
            // f=offset(무시). 실패하면 POSIX 관례상 (void*)-1을
            // 반환해야 하지만, 이 커널의 유저 주소공간에서 0은 절대
            // 유효한 매핑 시작점이 아니므로 0을 실패로 쓴다 — musl의
            // mmap() 래퍼(src/mman/mmap.c)가 이 값을 그대로
            // MAP_FAILED 판정에 쓰지 않고 __syscall_ret을 거치므로,
            // 0을 그대로 돌려주면 "성공, 주소=0"으로 오인될 수 있다
            // — 그래서 실패 시 -12(ENOMEM 음수)를 대신 돌려준다.
            uint64_t vaddr = mc_mmap_anon((uint64_t)b);
            return vaddr != 0 ? (long)vaddr : -12;
        }
        case SYS_munmap: {
            mc_munmap((uint64_t)a, (uint64_t)b);
            return 0;
        }
        default:
            log_unimplemented(n);
            return MC_ENOSYS;
    }
}
