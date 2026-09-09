// servers/vfs/main.cpp — VFS 서버 (docs/plan/system-servers-bringup.md
// §M13, docs/spec/fs-protocol.md, ADR-018).
//
// M13 최소 버전 — 마운트 테이블이 없다(단일 memfs만 존재) — 그래서
// OP_OPEN을 받으면 경로를 그대로 memfs에게 다시 보내고, 성공하면
// ADR-018이 요구하는 대로 memfs의 endpoint에 대한 프록시 핸들을
// 응답(handles[0])에 실어 클라이언트에게 위임한다. 그 이후 클라이언트는
// WRITE/READ를 memfs에게 직접 보낸다 — vfs는 다시 거치지 않는다.
#include <uapi.hpp>

namespace {

// sys_process_spawn(create_endpoint=true, ADR-152)이 handle 1에 이
// 프로세스의 endpoint를 만들어 준다. handle 2는 initrun이 스폰 시점에
// inherited_handles로 넣어 준 memfs endpoint 프록시다(lib/*.ini의
// `depends=memfs`, init/initrun/main.cpp 참고) — vfs 자신이 memfs의
// 클라이언트가 되는 유일한 자리.
constexpr uint32_t k_own_endpoint_handle = 1;
constexpr uint32_t k_memfs_handle = 2;

constexpr uint32_t k_op_open = 1;
constexpr uint64_t k_status_ok = 0;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

void handle_open(const uapi::message& in, uapi::message& out) {
    uapi::message fwd_in{};
    fwd_in.label = k_op_open;
    for (uint32_t i = 0; i < uapi::k_message_registers; ++i) {
        fwd_in.regs[i] = in.regs[i];
    }
    uapi::message fwd_out{};
    do_syscall(uapi::k_syscall_ipc_call, k_memfs_handle, reinterpret_cast<uint64_t>(&fwd_in),
               reinterpret_cast<uint64_t>(&fwd_out));

    out.regs[0] = fwd_out.regs[0];  // open_file_id(memfs가 발급).
    out.regs[1] = fwd_out.regs[1];  // 상태.
    if (fwd_out.regs[1] == k_status_ok) {
        // ADR-018 — open() 성공 응답에 대상 FS 서버(memfs) 핸들을
        // 위임한다. ADR-151이 sys_reply의 handles[] 전달을 가능하게
        // 해 뒀다 — src_handle은 **vfs 자신의** memfs 핸들 번호(2)다.
        out.handle_count = 1;
        out.handles[0].src_handle = k_memfs_handle;
        out.handles[0].rights_mask = uapi::k_right_can_send;
    }
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    for (;;) {
        uapi::message in{};
        uint64_t recv_err = do_syscall(uapi::k_syscall_ipc_recv, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in), 0);
        uapi::message out{};
        if (recv_err == 0) {
            out.label = in.label;
            if (in.label == k_op_open) {
                handle_open(in, out);
            }
        }
        do_syscall(uapi::k_syscall_ipc_reply, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
