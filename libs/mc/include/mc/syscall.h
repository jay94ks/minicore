// libs/mc/include/mc/syscall.h — 커널 syscall ABI의 단일 출처
// (docs/design/foundations.md ADR-132/170/200).
//
// ADR-200 이전에는 이 파일이 kernel/include/uapi.hpp를 손으로 베껴
// 유지하는 "커널 syscall ABI에 대한 C 바인딩"이었다 — 정본은 여전히
// uapi.hpp였고, 둘이 바이트 단위로 맞아야 한다는 책임은 사람(주석)
// 에게만 있었다. ADR-200이 그 예외를 없앤다: 이제 이 파일 자체가
// 유일한 정본이고, uapi.hpp는 폐지됐다.
//
// **커널-랜드/유저-랜드 구분(`MC_LAND_KERNEL`)**: 이 헤더를 include
// 하기 전에 `MC_LAND_KERNEL`을 정의하면(커널 빌드가 전역으로 정의,
// kernel/CMakeLists.txt 참고) 구조체/enum 대응 상수만 노출되고
// syscall 트램폴린 함수(`mc_raw_syscall`/`mc_ipc_call` 등)는
// 숨겨진다 — 커널은 자기 자신을 syscall로 호출할 이유가 없다.
// 매크로가 없으면(기본값, 유저랜드) 지금까지와 동일하게 전부
// 노출된다.
//
// 순수 C(+POD 구조체)로만 작성돼 있어 C++(커널, ADR-010)와 C
// (포팅된 libc, ADR-183) 양쪽에서 그대로 유효하다 — 별도 기법이
// 필요 없다(ADR-132 §결정2가 이미 확립한 패턴).
#pragma once

#include <stdint.h>

// ── 메시지 레이아웃 (kern::ipc::message와 바이트 단위로 동일해야 한다) ──

#define MC_MESSAGE_REGISTERS 4u
#define MC_MAX_PAGE_DESCRIPTORS 4u
#define MC_MAX_HANDLE_TRANSFERS 2u

#define MC_TRANSFER_COPY 0u
#define MC_TRANSFER_MOVE 1u
#define MC_TRANSFER_MAP 2u

typedef struct {
    uint64_t vaddr;
    uint64_t length;
    uint8_t mode;  // MC_TRANSFER_*.
} mc_page_descriptor;

typedef struct {
    uint32_t src_handle;
    uint32_t rights_mask;
} mc_handle_transfer;

typedef struct {
    uint32_t label;
    uint32_t page_count;
    uint32_t handle_count;
    uint64_t regs[MC_MESSAGE_REGISTERS];
    mc_page_descriptor pages[MC_MAX_PAGE_DESCRIPTORS];
    mc_handle_transfer handles[MC_MAX_HANDLE_TRANSFERS];
} mc_message;

// object::k_right_can_*(kern::object, kernel_objects.hpp)와 정확히
// 같은 비트값 — handle_transfer.rights_mask에 쓴다.
#define MC_RIGHT_CAN_SEND (1u << 0)
#define MC_RIGHT_CAN_RECV (1u << 1)
#define MC_RIGHT_CAN_MOVE (1u << 2)
#define MC_RIGHT_CAN_MAP (1u << 3)
#define MC_RIGHT_CAN_KILL (1u << 4)

// ── syscall 번호 ──

#define MC_SYSCALL_IPC_CALL 0u
#define MC_SYSCALL_PROCESS_SPAWN 1u
#define MC_SYSCALL_FORK 2u
#define MC_SYSCALL_EXEC 3u
#define MC_SYSCALL_THREAD_EXIT 4u
#define MC_SYSCALL_ALLOC_DMA_BUFFER 5u
#define MC_SYSCALL_IPC_RECV 6u
#define MC_SYSCALL_IPC_REPLY 7u
#define MC_SYSCALL_DEBUG_LOG 8u
#define MC_SYSCALL_MAP_PHYS 9u
#define MC_SYSCALL_IO_ACTIVATE 10u
#define MC_SYSCALL_IO_DEACTIVATE 11u
#define MC_SYSCALL_PROCESS_KILL 12u
#define MC_SYSCALL_BRK 13u
#define MC_SYSCALL_ARCH_PRCTL_SET_FS 14u
#define MC_SYSCALL_MMAP_ANON 15u
#define MC_SYSCALL_MUNMAP 16u
#define MC_SYSCALL_PROCSRV_PID_GET 17u
#define MC_SYSCALL_PROCSRV_PID_SET 18u
#define MC_SYSCALL_SIGNAL_ACTION 19u
#define MC_SYSCALL_SIGNAL_SEND 20u
#define MC_SYSCALL_RT_SIGRETURN 21u
// M36(real-libc-syscall-layer.md §M36) — sys_yield(인자 없음, 항상 0
// 반환). kern::sched::yield()를 유저랜드에 그대로 노출한다 — 자기
// 자신을 run queue에 다시 넣고 다른 runnable 스레드에게 이 코어를
// 양보한다. musl의 SYS_sched_yield가 이 자리를 통해 우회한다
// (syscall_shim.c). fork()로 COW 분리된 자식과 부모 사이에는 공유
// 메모리로 "준비됐다" 신호를 주고받을 방법이 없어(각자 쓰기는
// 자기 사본에만 반영된다) — 협조가 필요한 자기테스트(예: 자식이
// sigaction()을 마칠 시간을 실제로 벌어 주는 것)가 대신 이걸
// 반복 호출해 스케줄러가 다른 스레드를 실행할 기회를 준다(musl-hello
// 참고). getpid() 같은 캐시된 syscall은 실제로 스케줄러를 건드리지
// 않아(카드가 이미 유저랜드에 있음) 이 목적에 안 맞는다는 것을 실제로
// 겪었다(2026-09-10).
#define MC_SYSCALL_YIELD 22u

#define MC_MAX_DEBUG_LOG_BYTES 96u
#define MC_MAX_MMIO_MAP_BYTES (16ull * 1024 * 1024)

// sys_process_spawn(a1 = 이 구조체의 유저 가상주소). 반환값 0=성공,
// 그 외 process_ops.hpp::process_spawn_error 값.
#define MC_MAX_SPAWN_INHERITED_HANDLES 4u

typedef struct {
    uint64_t elf_data;
    uint64_t elf_size;
    uint64_t argv_blob;  // NUL로 구분된 문자열들이 이어진 블록, 마지막도 NUL. 0=인자 없음.
    uint64_t argv_size;
    uint8_t grant_trusted;  // bool.

    uint8_t create_endpoint;         // bool.
    uint32_t out_endpoint_proxy_handle;  // 출력.

    uint32_t inherited_handle_count;
    mc_handle_transfer inherited_handles[MC_MAX_SPAWN_INHERITED_HANDLES];

    uint32_t out_thread_handle;  // 출력(M22, ADR-178) — k_right_can_kill만 부여.

    // M28(real-libc-syscall-layer.md §M28, ADR-183) — true면 유저 스택
    // 최상단에 Linux ABI 초기 스택(argc/argv/envp/auxv)을 구성한다
    // (musl의 crt_arch.h가 요구). false(기본)면 기존 arg0 관례만 쓴다.
    uint8_t linux_abi_stack;

    // M29(real-libc-syscall-layer.md §M29, ADR-189) — 0(기본)이면
    // 인터프리터 없음(M28과 동일한 정적 실행). 0이 아니면 이
    // 인터프리터(ET_DYN, 예: musl 자신의 ld-musl-x86_64.so.1)를
    // 별도 베이스에 추가로 적재하고, 실제 진입점을 인터프리터의
    // 것으로 바꾸며, auxv에 AT_PHDR/AT_PHENT/AT_PHNUM(주 프로그램의
    // 것)+AT_ENTRY(주 프로그램의 진짜 진입점)+AT_BASE(이 인터프리터의
    // 로드 바이어스)를 실제 값으로 채운다 — linux_abi_stack이
    // true일 때만 의미 있다(false면 무시된다).
    uint64_t interp_data;
    uint64_t interp_size;
} mc_process_spawn_request;

// sys_exec(a1 = 이 구조체의 유저 가상주소) — 성공하면 반환하지
// 않는다. 실패해야만 반환하고, 반환값은 process_spawn_error 값이다.
typedef struct {
    uint64_t elf_data;
    uint64_t elf_size;
    uint64_t argv_blob;
    uint64_t argv_size;

    // M32(real-libc-syscall-layer.md §M32) — mc_process_spawn_request::
    // linux_abi_stack과 완전히 같은 의미(M28의 Linux ABI 초기 스택,
    // build_process()의 기존 경로 재사용). false(기본, POD 구조체를
    // {}로 초기화하면 0)면 기존 M12 self-exec/M18 su-target 관례
    // 그대로다.
    uint8_t linux_abi_stack;
} mc_exec_request;

// M36(real-libc-syscall-layer.md §M36, ADR-186) — sys_signal_action
// (a1=시그널 번호(1~31), a2=이 구조체의 유저 가상주소). handler==0=
// SIG_DFL(이 라운드는 "무시"로 취급, 진짜 기본 동작은 범위 밖),
// handler==1=SIG_IGN. new_action이 채워져 있으면(set_new!=0) 새
// 등록으로 갈아 끼우고, want_old!=0이면 그 전 값을 out_old에
// 채운다(POSIX sigaction()의 oldact 관례). 항상 성공한다(SIGKILL/
// SIGSTOP류에 대한 거부는 syscall_shim.c가 musl 쪽에서 먼저 걸러도
// 되지만, 커널도 방어적으로 그 번호는 조용히 무시한다).
typedef struct {
    uint8_t set_new;
    uint64_t new_handler;
    uint64_t new_restorer;
    uint8_t want_old;
    uint64_t out_old_handler;   // 출력.
    uint64_t out_old_restorer;  // 출력.
} mc_signal_action_request;

// sys_fork(인자 없음) — 반환값: 자식에서는 0, 부모에서는 1(성공)
// 또는 process_spawn_error(실패).

// sys_alloc_dma_buffer(a1=이 구조체의 유저 가상주소 출력, a2=order) —
// trusted 전용.
typedef struct {
    uint64_t virt_addr;
    uint64_t phys_addr;
} mc_dma_buffer_result;

// sys_map_phys(a1=이 구조체의 유저 가상주소) — trusted 전용.
typedef struct {
    uint64_t phys_addr;      // 페이지 정렬 불필요.
    uint64_t size;
    uint64_t out_virt_addr;  // 출력.
} mc_map_phys_request;

// sys_io_activate(a1=io_base, a2=count) / sys_io_deactivate(인자 없음)
// — trusted 전용.

// sys_process_kill(a1=대상 object_kind::thread 핸들, MC_RIGHT_CAN_KILL
// 필요) — 반환값 0=성공(요청 접수), 그 외 process_kill_error 값.

// sys_brk(a1=이 구조체의 유저 가상주소) — increment==0이면 조회만.
// 음수(축소)는 이번 라운드에 지원하지 않는다(invalid_argument로 거부).
typedef struct {
    int64_t increment;
    uint64_t out_old_top;  // 출력 — 증가 전 heap_top(sbrk() 관례).
} mc_brk_request;

// sys_arch_prctl_set_fs(a1=fs_base) — musl의 SYS_arch_prctl(ARCH_SET_FS,
// addr) 번역 대상(M28, real-libc-syscall-layer.md §M28). Linux의
// arch_prctl은 여러 code(ARCH_SET_FS/GET_FS/SET_GS/GET_GS)를 하나의
// syscall로 다루지만, 이 커널은 musl 시작 경로가 실제로 쓰는 경우
// (ARCH_SET_FS)만 전용 syscall로 노출한다(ADR-001 — 정확성 우선,
// 안 쓰는 경우를 미리 일반화하지 않는다). 항상 성공(0)한다 — 실패
// 조건이 없다(호출 스레드 자신의 MSR을 즉시 쓴다).

// sys_procsrv_pid_get(인자 없음)/sys_procsrv_pid_set(a1=pid) — 이
// 스레드의 kern::object::thread::procsrv_pid를 읽고 쓴다(M32,
// real-libc-syscall-layer.md §M32). 커널은 이 값의 의미를 모른다 —
// procsrv가 부여한 pid를 exec()을 거쳐도 잃지 않게 스레드 객체에
// 저장해 두는 순수 스토리지 역할뿐이다(kernel_objects.hpp::
// thread::procsrv_pid 주석 참고). 둘 다 항상 성공한다(실패 조건이
// 없다 — 호출 스레드 자신의 필드를 즉시 읽고 쓴다).

// M12 self-test 임시 배선 — kernel_main.cpp::setup_initrun_process와
// procsrv가 자기 자신을 fork/spawn/exec으로 다시 만들어 보는 데
// 쓴다. **procsrv/cpio가 실제로 생긴 뒤에도 여전히 실사용 중이다**
// (2026-09-10 ADR-200 실행 시점에 재확인 — servers/procsrv/main.cpp
// 의 run_loader_test가 이 구조체를 그대로 참조한다. "procsrv/cpio가
// 생기면 통째로 제거된다"던 uapi.hpp의 예전 주석은 실현되지 않았다).
#define MC_M12_SELF_ELF_USER_VADDR 0x0000700000003000ull
#define MC_M12_SELF_INFO_USER_VADDR 0x0000700000100000ull

typedef struct {
    uint64_t elf_addr;
    uint64_t elf_size;
} mc_m12_self_info;

#ifndef MC_LAND_KERNEL

// ── 아래부터는 유저-랜드 전용: syscall 트램폴린 + 얇은 래퍼 ──
// (커널-랜드에서는 숨긴다 — 커널은 이 함수들을 호출할 이유가 없다.)

static inline uint64_t mc_raw_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t mc_ipc_call(uint32_t handle, mc_message* in, mc_message* out) {
    return mc_raw_syscall(MC_SYSCALL_IPC_CALL, handle, (uint64_t)(uintptr_t)in,
                          (uint64_t)(uintptr_t)out);
}

static inline uint64_t mc_ipc_recv(uint32_t handle, mc_message* msg) {
    return mc_raw_syscall(MC_SYSCALL_IPC_RECV, handle, (uint64_t)(uintptr_t)msg, 0);
}

static inline uint64_t mc_ipc_reply(mc_message* msg) {
    return mc_raw_syscall(MC_SYSCALL_IPC_REPLY, (uint64_t)(uintptr_t)msg, 0, 0);
}

static inline void mc_debug_log(const char* msg, uint64_t len) {
    mc_raw_syscall(MC_SYSCALL_DEBUG_LOG, (uint64_t)(uintptr_t)msg, len, 0);
}

// 절대 반환하지 않는다(sched::exit() 그대로 노출).
static inline _Noreturn void mc_thread_exit(void) {
    for (;;) {
        mc_raw_syscall(MC_SYSCALL_THREAD_EXIT, 0, 0, 0);
        __asm__ volatile("pause");
    }
}

// M24(general-purpose-completion.md §M24, ADR-180) — sys_brk.
// 반환: 0=성공(req.out_old_top에 증가 전 heap_top이 채워진다),
// 그 외는 process_ops.hpp::process_spawn_error 값(대개
// out_of_memory — 힙 슬롯 예산 초과).
static inline uint64_t mc_brk(mc_brk_request* req) {
    return mc_raw_syscall(MC_SYSCALL_BRK, (uint64_t)(uintptr_t)req, 0, 0);
}

// M28(real-libc-syscall-layer.md §M28) — sys_arch_prctl_set_fs.
static inline uint64_t mc_arch_prctl_set_fs(uint64_t fs_base) {
    return mc_raw_syscall(MC_SYSCALL_ARCH_PRCTL_SET_FS, fs_base, 0, 0);
}

// M30(real-libc-syscall-layer.md §M30) — sys_mmap_anon/sys_munmap.
// mc_mmap_anon 반환값 0은 실패(이 커널의 유저 주소공간에서 0은
// 절대 유효한 매핑 시작점이 될 수 없다 — sys_brk의 heap_top==0
// sentinel과 같은 근거).
static inline uint64_t mc_mmap_anon(uint64_t size) {
    return mc_raw_syscall(MC_SYSCALL_MMAP_ANON, size, 0, 0);
}

static inline uint64_t mc_munmap(uint64_t addr, uint64_t size) {
    return mc_raw_syscall(MC_SYSCALL_MUNMAP, addr, size, 0);
}

// M32(real-libc-syscall-layer.md §M32) — sys_procsrv_pid_get/set.
static inline uint32_t mc_procsrv_pid_get(void) {
    return (uint32_t)mc_raw_syscall(MC_SYSCALL_PROCSRV_PID_GET, 0, 0, 0);
}

static inline void mc_procsrv_pid_set(uint32_t pid) {
    mc_raw_syscall(MC_SYSCALL_PROCSRV_PID_SET, pid, 0, 0);
}

// M32(real-libc-syscall-layer.md §M32) — sys_exec(기존 M12부터 있던
// syscall, ADR-183 §결정4가 요구하는 "새 Linux syscall(SYS_execve)은
// 먼저 libmc 얇은 래퍼로" 원칙을 여기서 지킨다). IPC/프로토콜 로직이
// 전혀 없는 순수 syscall 트램폴린이라 procsrv_client.h가 아니라 다른
// mc_mmap_anon류와 같은 자리에 둔다. 성공하면 반환하지 않는다 —
// 실패해야만 반환하고, 반환값은 process_ops.hpp::process_spawn_error
// 값이다.
static inline uint64_t mc_exec(uint64_t elf_data, uint64_t elf_size, uint64_t argv_blob,
                                uint64_t argv_size, uint8_t linux_abi_stack) {
    mc_exec_request req;
    req.elf_data = elf_data;
    req.elf_size = elf_size;
    req.argv_blob = argv_blob;
    req.argv_size = argv_size;
    req.linux_abi_stack = linux_abi_stack;
    return mc_raw_syscall(MC_SYSCALL_EXEC, (uint64_t)(uintptr_t)&req, 0, 0);
}

// M36(real-libc-syscall-layer.md §M36, ADR-186) — sys_signal_action/
// sys_signal_send/sys_rt_sigreturn. 셋 다 IPC/프로토콜 로직이 없는
// 순수 syscall 트램폴린이라(procsrv를 거치지 않는다 — 대상은 항상
// 호출자가 이미 들고 있는 object_kind::thread 핸들이다)
// procsrv_client.h가 아니라 mc_exec류와 같은 자리에 둔다.
static inline uint64_t mc_signal_action(uint32_t signal_number, uint8_t set_new,
                                         uint64_t new_handler, uint64_t new_restorer,
                                         uint8_t want_old, uint64_t* out_old_handler,
                                         uint64_t* out_old_restorer) {
    mc_signal_action_request req;
    req.set_new = set_new;
    req.new_handler = new_handler;
    req.new_restorer = new_restorer;
    req.want_old = want_old;
    req.out_old_handler = 0;
    req.out_old_restorer = 0;
    uint64_t ret = mc_raw_syscall(MC_SYSCALL_SIGNAL_ACTION, signal_number,
                                   (uint64_t)(uintptr_t)&req, 0);
    if (out_old_handler != 0) {
        *out_old_handler = req.out_old_handler;
    }
    if (out_old_restorer != 0) {
        *out_old_restorer = req.out_old_restorer;
    }
    return ret;
}

// target_thread_handle은 MC_RIGHT_CAN_SIGNAL(=MC_RIGHT_CAN_KILL과
// 같은 비트)이 필요하다 — sys_process_kill(ADR-178)이 이미 발급하는
// 그 핸들을 그대로 쓴다. SIGKILL(9)은 이 경로를 거부한다(항상
// process_kill_error 계열의 음수 아닌 실패 코드) — sys_process_kill
// 을 대신 쓴다(ADR-186 §결정4, 일반화하지 않는다).
static inline uint64_t mc_signal_send(uint32_t target_thread_handle, uint32_t signal_number) {
    return mc_raw_syscall(MC_SYSCALL_SIGNAL_SEND, target_thread_handle, signal_number, 0);
}

// 시그널 핸들러가 반환한 뒤(restorer 트램폴린이 부른다) 원래
// 실행으로 복귀한다 — 정상적으로는 반환하지 않는다(syscall_entry.S
// 의 saved_regs를 그 자리에서 다시 덮어써 버린다).
static inline uint64_t mc_sigreturn(void) {
    return mc_raw_syscall(MC_SYSCALL_RT_SIGRETURN, 0, 0, 0);
}

static inline uint64_t mc_yield(void) {
    return mc_raw_syscall(MC_SYSCALL_YIELD, 0, 0, 0);
}

#endif  // !MC_LAND_KERNEL
