// libmc/include/mc/syscall.h — 커널 syscall ABI에 대한 1:1 C 바인딩
// (docs/design/foundations.md ADR-132/170).
//
// kernel/include/uapi.hpp와 바이트 단위로 동일한 레이아웃이어야 한다
// (필드 순서·타입이 정확히 대응). 이번 라운드(M20)는 userland/shell이
// 실제로 쓰는 syscall만 감싼다 — sys_fork/sys_exec/sys_process_spawn/
// sys_alloc_dma_buffer/sys_map_phys/sys_io_activate류는 아직 이
// 라이브러리의 어떤 클라이언트도 필요로 하지 않아 비워 둔다(다음에
// 실제로 필요해지는 시점에 추가, ADR-170 §영향).
#pragma once

#include <stdint.h>

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

// uapi::message과 정확히 대응(label/page_count/handle_count/regs[]/
// pages[]/handles[]).
typedef struct {
    uint32_t label;
    uint32_t page_count;
    uint32_t handle_count;
    uint64_t regs[MC_MESSAGE_REGISTERS];
    mc_page_descriptor pages[MC_MAX_PAGE_DESCRIPTORS];
    mc_handle_transfer handles[MC_MAX_HANDLE_TRANSFERS];
} mc_message;

#define MC_SYSCALL_IPC_CALL 0u
#define MC_SYSCALL_IPC_RECV 6u
#define MC_SYSCALL_IPC_REPLY 7u
#define MC_SYSCALL_DEBUG_LOG 8u

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

#define MC_MAX_DEBUG_LOG_BYTES 96u

static inline void mc_debug_log(const char* msg, uint64_t len) {
    mc_raw_syscall(MC_SYSCALL_DEBUG_LOG, (uint64_t)(uintptr_t)msg, len, 0);
}

// sys_thread_exit — 절대 반환하지 않는다(kernel/include/uapi.hpp의
// k_syscall_thread_exit 그대로).
static inline _Noreturn void mc_thread_exit(void) {
    for (;;) {
        mc_raw_syscall(4u /* k_syscall_thread_exit */, 0, 0, 0);
        __asm__ volatile("pause");
    }
}
