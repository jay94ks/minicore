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
#include <mc/fs_client.h>
#include <mc/syscall.h>
#include <mc/vfs_client.h>
#include <sys/uio.h>

#define SYS_open 2
#define SYS_write 1
#define SYS_close 3
#define SYS_fstat 5
#define SYS_lseek 8
#define SYS_mmap 9
#define SYS_ioctl 16
#define SYS_readv 19
#define SYS_writev 20
#define SYS_munmap 11
#define SYS_brk 12
#define SYS_exit 60
#define SYS_read 0
#define SYS_arch_prctl 158
#define SYS_exit_group 231
#define SYS_openat 257

#define ARCH_SET_FS 0x1002

#define MC_EBADF (-9)
#define MC_ENOENT (-2)
#define MC_ENOTTY (-25)
#define MC_ENOSYS (-38)

// M31(real-libc-syscall-layer.md §M31, ADR-183) — VFS로 연 파일의
// fd(int)↔{fs_handle, open_file_id} 대응표. fd 0/1/2는 이 표에
// 들어오지 않는다(stdin/stdout/stderr는 기존 SYS_write 경로,
// mc_debug_log로 처리 — VFS와 무관). 이 프로젝트에 아직 진짜
// "fd 진실 공급원"이 없다(procsrv.md §3.6, OPEN-64에서 이미 범위
// 밖으로 남겨 둔 부분)는 것과 같은 정신으로, 이 표는 **이
// 프로세스 하나만** 아는 로컬 상태다 — fork() 시 상속되지 않는다
// (이 라운드는 fork+파일 공유를 다루지 않는다, M32 이후 대상).
#define MC_MAX_OPEN_FILES 8
static struct {
    int in_use;
    uint32_t fs_handle;
    uint64_t open_file_id;
} g_open_files[MC_MAX_OPEN_FILES];

// VFS 클라이언트 핸들 — musl-hello가 depends=vfs로 initrun에게서
// 물려받는다(servers/CMakeLists.txt, --depends=musl-hello:vfs).
// create_endpoint=true가 항상 handle 1을 먼저 차지하므로(ADR-152의
// 고정 순서), 상속된 첫 핸들은 handle 2가 된다 — 다른 모든 서비스
// (procsrv/login 등)가 vfs를 받을 때와 같은 관례.
#define MC_VFS_HANDLE 2

static long open_common(const char* path) {
    int slot = -1;
    for (int i = 0; i < MC_MAX_OPEN_FILES; ++i) {
        if (!g_open_files[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return MC_ENOENT;  // 표가 가득 찼다 — 이 라운드 예산(8개)을 넘음.
    }
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    uint64_t status = mc_vfs_open(MC_VFS_HANDLE, path, /*identity=*/0, &open_file_id, &fs_handle);
    if (status != MC_FS_STATUS_OK || fs_handle == 0) {
        return MC_ENOENT;
    }
    g_open_files[slot].in_use = 1;
    g_open_files[slot].fs_handle = fs_handle;
    g_open_files[slot].open_file_id = open_file_id;
    return slot + 3;  // fd 0/1/2는 예약.
}

static long read_common(long fd, void* buf, unsigned long count) {
    if (fd < 3 || fd - 3 >= MC_MAX_OPEN_FILES || !g_open_files[fd - 3].in_use) {
        return MC_EBADF;
    }
    uint64_t n = mc_fs_read(g_open_files[fd - 3].fs_handle, g_open_files[fd - 3].open_file_id,
                             (uint8_t*)buf, count);
    return (long)n;
}

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
        // M31(real-libc-syscall-layer.md §M31) — 파일 I/O. open_common/
        // read_common(위)이 VFS 프로토콜 클라이언트(libmc의
        // mc_vfs_open/mc_fs_read, M13~M20이 이미 만들어 둔 것)를
        // 그대로 부른다 — 이 파일 자신은 POSIX 인자(dirfd/flags/
        // iovec 등)를 그 함수들이 받는 형태로 번역만 한다(ADR-183
        // §결정4).
        case SYS_open: {
            const char* path = (const char*)(uintptr_t)a;
            return open_common(path);
        }
        case SYS_openat: {
            // b=path — a(dirfd)는 무시한다(이 라운드는 AT_FDCWD류
            // 상대 경로 해석을 다루지 않는다, VFS 경로는 항상
            // 절대경로로 취급).
            const char* path = (const char*)(uintptr_t)b;
            return open_common(path);
        }
        case SYS_read: {
            return read_common(a, (void*)(uintptr_t)b, (unsigned long)c);
        }
        case SYS_readv: {
            struct iovec* iov = (struct iovec*)(uintptr_t)b;
            long iovcnt = c;
            long total = 0;
            for (long i = 0; i < iovcnt; ++i) {
                if (iov[i].iov_len == 0) {
                    continue;
                }
                long got = read_common(a, iov[i].iov_base, iov[i].iov_len);
                if (got < 0) {
                    return total > 0 ? total : got;
                }
                total += got;
                if ((unsigned long)got < iov[i].iov_len) {
                    break;  // 부분 읽기 또는 EOF — readv 관례상 더 시도하지 않는다.
                }
            }
            return total;
        }
        case SYS_close: {
            long fd = a;
            if (fd >= 3 && fd - 3 < MC_MAX_OPEN_FILES) {
                g_open_files[fd - 3].in_use = 0;
            }
            return 0;
        }
        case SYS_lseek:
            // 이 프로젝트의 fs-protocol.md에는 아직 "커서를 임의
            // 위치로 옮기는" 오퍼레이션이 없다(읽기는 항상 서버
            // 쪽 커서를 순차 전진만 시킨다, M23의 fd 상속 테스트가
            // 이미 이 전제로 검증됐다) — 알려진 단순화로 항상
            // 실패시킨다(musl의 순차 fread/fclose 경로는 seek를
            // 요구하지 않는다).
            return MC_ENOSYS;
        case SYS_fstat:
            // 스텁 — 이 라운드는 파일 크기/타입 조회가 필요 없다
            // (순차 fread만 검증). 항상 실패시킨다.
            return MC_ENOSYS;
        case SYS_ioctl: {
            // 스텁 — "isatty 판별용"(계획 텍스트). TIOCGWINSZ를
            // 항상 실패시켜(진짜 터미널이 아니다) musl의 stdio가
            // stdout을 완전 버퍼링 모드로 두게 만든다(__stdout_write.c
            // 참고) — 그 외 ioctl 요청도 전부 실패.
            return MC_ENOTTY;
        }
        case SYS_writev: {
            long fd = a;
            if (fd != 1 && fd != 2) {
                return MC_EBADF;  // VFS 쓰기는 이 라운드 범위 밖(읽기만 검증).
            }
            struct iovec* iov = (struct iovec*)(uintptr_t)b;
            long iovcnt = c;
            long total = 0;
            for (long i = 0; i < iovcnt; ++i) {
                unsigned long len = iov[i].iov_len;
                if (len > MC_MAX_DEBUG_LOG_BYTES) {
                    len = MC_MAX_DEBUG_LOG_BYTES;  // mc_debug_log 자신의 한 번 호출 한도.
                }
                mc_debug_log((const char*)iov[i].iov_base, len);
                total += (long)len;
            }
            return total;
        }
        default:
            log_unimplemented(n);
            return MC_ENOSYS;
    }
}
