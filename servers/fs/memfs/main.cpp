// servers/fs/memfs/main.cpp — 메모리 파일시스템 서버
// (docs/plan/system-servers-bringup.md §M13, docs/spec/fs-protocol.md).
//
// M13 최소 버전 — 고정 개수의 이름 있는 파일 슬롯(디렉터리 없음, 평평한
// 이름공간)과, 그 슬롯을 가리키는 "열린 인스턴스" 배열(open_file_id)만
// 있다. OPEN은 vfs가 대신 호출해 준다(ADR-018) — memfs 자신은 마운트
// 개념도, 경로 정규화도 모른다. WRITE/READ는 그 이후 클라이언트가
// (vfs를 거치지 않고) memfs의 endpoint에 직접 건다.
#include <uapi.hpp>

namespace {

// sys_process_spawn(create_endpoint=true)가 항상 handle 1에 이
// 프로세스의 첫 endpoint를 만들어 준다(ADR-152) — memfs는 의존하는
// 서버가 없어 handle 2 이상은 쓰지 않는다.
constexpr uint32_t k_own_endpoint_handle = 1;

constexpr uint32_t k_op_open = 1;
constexpr uint32_t k_op_write = 2;
constexpr uint32_t k_op_read = 3;

constexpr uint64_t k_status_ok = 0;
constexpr uint64_t k_status_not_found = 1;
constexpr uint64_t k_status_too_large = 2;
constexpr uint64_t k_status_no_space = 3;

constexpr uint32_t k_max_files = 8;
constexpr uint32_t k_max_file_bytes = 4096;
constexpr uint32_t k_max_open_files = 16;
constexpr uint32_t k_max_io_bytes = 16;  // fs-protocol.md §1 — regs[2..3]에 담을 수 있는 상한.

struct file_slot {
    bool used = false;
    char name[32] = {};
    uint8_t data[k_max_file_bytes] = {};
    uint64_t size = 0;
};

struct open_instance {
    bool used = false;
    uint32_t file_index = 0;
};

file_slot g_files[k_max_files];
open_instance g_opens[k_max_open_files];

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

bool name_equals(const char* a, const char* b) {
    for (int i = 0; i < 32; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
        if (a[i] == '\0') {
            return true;
        }
    }
    return true;
}

uint32_t find_or_create_file(const char* name) {
    for (uint32_t i = 0; i < k_max_files; ++i) {
        if (g_files[i].used && name_equals(g_files[i].name, name)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < k_max_files; ++i) {
        if (!g_files[i].used) {
            g_files[i].used = true;
            int j = 0;
            for (; j < 31 && name[j] != '\0'; ++j) {
                g_files[i].name[j] = name[j];
            }
            g_files[i].name[j] = '\0';
            g_files[i].size = 0;
            return i;
        }
    }
    return k_max_files;  // 가득 참.
}

// open_file_id는 1부터 시작한다(0은 항상 무효 — kernel/core/object의
// handle 관례와 같은 정신, fs-protocol.md는 이 값의 의미를 memfs
// 내부 구현 재량으로 남겨 둔다).
uint32_t alloc_open_instance(uint32_t file_index) {
    for (uint32_t i = 0; i < k_max_open_files; ++i) {
        if (!g_opens[i].used) {
            g_opens[i].used = true;
            g_opens[i].file_index = file_index;
            return i + 1;
        }
    }
    return 0;
}

void handle_open(const uapi::message& in, uapi::message& out) {
    char path[33];
    __builtin_memcpy(path, in.regs, 32);
    path[32] = '\0';
    const char* name = (path[0] == '/') ? path + 1 : path;  // 평평한 이름공간 — 선행 '/'만 벗긴다.

    uint32_t file_index = find_or_create_file(name);
    if (file_index >= k_max_files) {
        out.regs[0] = 0;
        out.regs[1] = k_status_no_space;
        return;
    }
    uint32_t open_id = alloc_open_instance(file_index);
    out.regs[0] = open_id;
    out.regs[1] = (open_id != 0) ? k_status_ok : k_status_no_space;
}

void handle_write(const uapi::message& in, uapi::message& out) {
    uint32_t open_id = static_cast<uint32_t>(in.regs[0]);
    uint64_t length = in.regs[1];
    if (open_id == 0 || open_id > k_max_open_files || !g_opens[open_id - 1].used) {
        out.regs[0] = 0;
        out.regs[1] = k_status_not_found;
        return;
    }
    if (length > k_max_io_bytes) {
        out.regs[0] = 0;
        out.regs[1] = k_status_too_large;
        return;
    }
    file_slot& f = g_files[g_opens[open_id - 1].file_index];
    uint8_t buf[k_max_io_bytes];
    __builtin_memcpy(buf, &in.regs[2], k_max_io_bytes);
    for (uint64_t i = 0; i < length; ++i) {
        f.data[i] = buf[i];  // M13: 항상 오프셋 0부터(seek/append는 이후 라운드).
    }
    f.size = length;
    out.regs[0] = length;
    out.regs[1] = k_status_ok;
}

void handle_read(const uapi::message& in, uapi::message& out) {
    uint32_t open_id = static_cast<uint32_t>(in.regs[0]);
    uint64_t requested = in.regs[1];
    if (open_id == 0 || open_id > k_max_open_files || !g_opens[open_id - 1].used) {
        out.regs[0] = 0;
        out.regs[1] = k_status_not_found;
        return;
    }
    if (requested > k_max_io_bytes) {
        out.regs[0] = 0;
        out.regs[1] = k_status_too_large;
        return;
    }
    file_slot& f = g_files[g_opens[open_id - 1].file_index];
    uint64_t to_read = (requested < f.size) ? requested : f.size;
    uint8_t buf[k_max_io_bytes];
    for (uint64_t i = 0; i < k_max_io_bytes; ++i) {
        buf[i] = (i < to_read) ? f.data[i] : 0;
    }
    __builtin_memcpy(&out.regs[2], buf, k_max_io_bytes);
    out.regs[0] = to_read;
    out.regs[1] = k_status_ok;
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
            switch (in.label) {
                case k_op_open:
                    handle_open(in, out);
                    break;
                case k_op_write:
                    handle_write(in, out);
                    break;
                case k_op_read:
                    handle_read(in, out);
                    break;
                default:
                    out.regs[1] = k_status_not_found;
                    break;
            }
        }
        do_syscall(uapi::k_syscall_ipc_reply, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
