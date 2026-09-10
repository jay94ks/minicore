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
} mc_process_spawn_request;

// sys_exec(a1 = 이 구조체의 유저 가상주소) — 성공하면 반환하지
// 않는다. 실패해야만 반환하고, 반환값은 process_spawn_error 값이다.
typedef struct {
    uint64_t elf_data;
    uint64_t elf_size;
    uint64_t argv_blob;
    uint64_t argv_size;
} mc_exec_request;

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

#endif  // !MC_LAND_KERNEL
