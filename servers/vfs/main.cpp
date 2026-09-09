// servers/vfs/main.cpp — VFS 서버 (docs/plan/system-servers-bringup.md
// §M13/§M16, docs/spec/fs-protocol.md, ADR-018).
//
// M13은 마운트 테이블이 없었다(단일 memfs). M16이 fs-protocol.md v2
// §2.1의 정적 마운트 테이블을 추가한다 — `/mnt/fat32/`, `/mnt/ext4/`
// 접두사는 각각 FAT32/ext4 서버에게, 그 외(M13과 동일한 기본 경로)는
// memfs에게 그대로 전달한다. 접두사를 매칭하면 그 부분을 벗겨낸
// **나머지 경로**만 대상 FS 서버에게 넘긴다. OP_OPEN 성공 시 ADR-018이
// 요구하는 대로 그 FS 서버의 endpoint 프록시를 응답(handles[0])에
// 실어 클라이언트에게 위임한다 — 이후 클라이언트는 WRITE/READ를 그
// FS 서버에게 직접 보낸다(vfs를 다시 거치지 않음, M13과 동일).
#include <uapi.hpp>

namespace {

// sys_process_spawn(create_endpoint=true, ADR-152)이 handle 1에 이
// 프로세스의 endpoint를 만들어 준다. handle 2~4는 initrun이 스폰
// 시점에 inherited_handles로 넣어 준 memfs/fat32/ext4 endpoint
// 프록시다(lib/*.ini의 `depends=memfs,fat32,ext4`, 나열 순서 그대로
// 핸들 번호가 된다 — init/initrun/main.cpp의 콤마 분리 로직 참고).
constexpr uint32_t k_own_endpoint_handle = 1;
constexpr uint32_t k_memfs_handle = 2;
constexpr uint32_t k_fat32_handle = 3;
constexpr uint32_t k_ext4_handle = 4;

constexpr uint32_t k_op_open = 1;
constexpr uint64_t k_status_ok = 0;

struct mount_entry {
    const char* prefix;
    uint64_t prefix_len;
    uint32_t handle;
};

uint64_t cstr_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

// fs-protocol.md v2 §2.1의 표 그대로 — 순서가 매칭 우선순위다(가장
// 구체적인 접두사를 먼저 검사). 마지막 매칭(빈 접두사)은 항상 성공해
// memfs로 떨어진다 — M13의 "선행 '/'만 벗긴다" 기본 경로와 동일.
const mount_entry k_mount_table[] = {
    {"/mnt/fat32/", 11, k_fat32_handle},
    {"/mnt/ext4/", 10, k_ext4_handle},
    {"", 0, k_memfs_handle},
};
constexpr uint32_t k_mount_table_size = sizeof(k_mount_table) / sizeof(k_mount_table[0]);

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

bool starts_with(const char* path, const char* prefix, uint64_t prefix_len) {
    for (uint64_t i = 0; i < prefix_len; ++i) {
        if (path[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

void handle_open(const uapi::message& in, uapi::message& out) {
    char path[33];
    __builtin_memcpy(path, in.regs, 32);
    path[32] = '\0';
    uint64_t path_len = cstr_len(path);

    uint32_t target_handle = k_memfs_handle;
    const char* rest = (path[0] == '/') ? path + 1 : path;  // 기본(memfs) 경로 — M13과 동일.
    for (uint32_t i = 0; i < k_mount_table_size; ++i) {
        const mount_entry& m = k_mount_table[i];
        if (m.prefix_len == 0) {
            continue;  // 기본값은 이미 위에서 처리했다.
        }
        if (path_len >= m.prefix_len && starts_with(path, m.prefix, m.prefix_len)) {
            target_handle = m.handle;
            rest = path + m.prefix_len;
            break;
        }
    }

    // 가변 길이 __builtin_memset/memcpy는 이 freestanding 빌드에서
    // 실제 libc 심볼 호출로 낮춰져 링크에 실패한다 — 손으로 바이트
    // 루프를 쓴다(servers/fs/fat32/main.cpp와 같은 이유).
    uapi::message fwd_in{};
    fwd_in.label = k_op_open;
    auto* regs_bytes = reinterpret_cast<uint8_t*>(fwd_in.regs);
    for (uint64_t i = 0; i < sizeof(fwd_in.regs); ++i) {
        regs_bytes[i] = 0;
    }
    uint64_t rest_len = cstr_len(rest);
    if (rest_len > sizeof(fwd_in.regs)) {
        rest_len = sizeof(fwd_in.regs);
    }
    for (uint64_t i = 0; i < rest_len; ++i) {
        regs_bytes[i] = static_cast<uint8_t>(rest[i]);
    }
    uapi::message fwd_out{};
    do_syscall(uapi::k_syscall_ipc_call, target_handle, reinterpret_cast<uint64_t>(&fwd_in),
               reinterpret_cast<uint64_t>(&fwd_out));

    out.regs[0] = fwd_out.regs[0];  // open_file_id(대상 FS 서버가 발급).
    out.regs[1] = fwd_out.regs[1];  // 상태.
    if (fwd_out.regs[1] == k_status_ok) {
        // ADR-018 — open() 성공 응답에 대상 FS 서버 핸들을 위임한다.
        out.handle_count = 1;
        out.handles[0].src_handle = target_handle;
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
