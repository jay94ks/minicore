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
constexpr uint32_t k_op_list = 4;  // M20(fs-protocol.md v4, filesystem.md ADR-172) — 셸 ls 빌트인.

constexpr uint64_t k_status_ok = 0;
constexpr uint64_t k_status_not_found = 1;
constexpr uint64_t k_status_no_space = 3;

constexpr uint32_t k_max_files = 8;
// M18(fs-protocol.md v3, security-model.md ADR-167) — su/sudo 로더가
// procsrv 자신의 ELF(수십 KiB)를 여기 써야 해서 4096→131072로
// 늘렸다.
constexpr uint32_t k_max_file_bytes = 131072;
constexpr uint32_t k_max_open_files = 16;
constexpr uint64_t k_page_size = 4096;

// M16(fs-protocol.md v2 §2.3, ADR-155 §2/ADR-159/ADR-161) — OP_READ
// 응답이 이제 pages[]로 내용을 옮긴다. 커널이 이 정확한 물리 프레임을
// 그대로 클라이언트에게 매핑하므로, file_slot::data처럼 다른 필드와
// 한 페이지를 공유하는 버퍼를 그대로 노출하면 클라이언트가 그 이웃
// 필드(다른 파일의 내용 등)까지 함께 보게 된다 — 그래서 읽기 응답
// 전용의 독립된 페이지 정렬 버퍼를 하나 둔다(요청마다 이 버퍼에
// 복사해 담은 뒤 그 프레임을 넘긴다).
alignas(k_page_size) uint8_t g_read_scratch[k_page_size] = {};

struct file_slot {
    bool used = false;
    char name[32] = {};
    uint8_t data[k_max_file_bytes] = {};
    uint64_t size = 0;
};

// M18(fs-protocol.md v3) — 읽기/쓰기 커서. open 시점에 둘 다 0으로
// 시작해, OP_READ/OP_WRITE가 매번 그 위치부터 이어서 전진시킨다
// (명시적 seek는 없다 — 순차 접근만).
struct open_instance {
    bool used = false;
    uint32_t file_index = 0;
    uint64_t read_cursor = 0;
    uint64_t write_cursor = 0;
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
            g_opens[i].read_cursor = 0;
            g_opens[i].write_cursor = 0;
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

// fs-protocol.md v3 §2.2 — 요청이 이제 pages[]로 온다(M13 시절엔
// regs[2..3] 16바이트 상한). write_cursor 위치부터 이어 쓰고 그만큼
// 전진시킨다 — seek는 없다(§4).
void handle_write(const uapi::message& in, uapi::message& out) {
    uint32_t open_id = static_cast<uint32_t>(in.regs[0]);
    uint64_t length = in.regs[1];
    if (open_id == 0 || open_id > k_max_open_files || !g_opens[open_id - 1].used) {
        out.regs[0] = 0;
        out.regs[1] = k_status_not_found;
        return;
    }
    if (length > k_page_size) {
        length = k_page_size;
    }
    open_instance& o = g_opens[open_id - 1];
    file_slot& f = g_files[o.file_index];
    if (o.write_cursor + length > k_max_file_bytes) {
        length = (o.write_cursor < k_max_file_bytes) ? (k_max_file_bytes - o.write_cursor) : 0;
    }
    if (in.page_count == 1 && length > 0) {
        const auto* src = reinterpret_cast<const uint8_t*>(in.pages[0].vaddr);
        for (uint64_t i = 0; i < length; ++i) {
            f.data[o.write_cursor + i] = src[i];
        }
    }
    o.write_cursor += length;
    if (o.write_cursor > f.size) {
        f.size = o.write_cursor;
    }
    out.regs[0] = length;
    out.regs[1] = k_status_ok;
}

// fs-protocol.md v3 §2.3 — read_cursor 위치부터 이어 읽고 그만큼
// 전진시킨다(M16에서는 항상 오프셋 0이었다). 요청 길이는 4096(한
// 페이지)으로 클램프될 뿐 TOO_LARGE로 거부하지 않는다.
void handle_read(const uapi::message& in, uapi::message& out) {
    uint32_t open_id = static_cast<uint32_t>(in.regs[0]);
    uint64_t requested = in.regs[1];
    if (open_id == 0 || open_id > k_max_open_files || !g_opens[open_id - 1].used) {
        out.regs[0] = 0;
        out.regs[1] = k_status_not_found;
        return;
    }
    if (requested > k_page_size) {
        requested = k_page_size;
    }
    open_instance& o = g_opens[open_id - 1];
    file_slot& f = g_files[o.file_index];
    uint64_t remaining = (o.read_cursor < f.size) ? (f.size - o.read_cursor) : 0;
    uint64_t to_read = (requested < remaining) ? requested : remaining;

    for (uint64_t i = 0; i < k_page_size; ++i) {
        g_read_scratch[i] = (i < to_read) ? f.data[o.read_cursor + i] : 0;
    }
    o.read_cursor += to_read;
    out.page_count = 1;
    out.pages[0].vaddr = reinterpret_cast<uint64_t>(g_read_scratch);
    out.pages[0].length = k_page_size;
    out.pages[0].mode = uapi::transfer_mode::copy;
    out.regs[0] = to_read;
    out.regs[1] = k_status_ok;
}

// fs-protocol.md v4 §2.4(filesystem.md ADR-172) — OP_WRITE/OP_READ와
// 같은 정신으로 VFS를 거치지 않고 클라이언트가 직접 부른다. 채워진
// 파일 슬롯 이름을 NUL로 구분해 한 페이지에 눌러 담는다.
void handle_list(const uapi::message&, uapi::message& out) {
    for (uint64_t i = 0; i < k_page_size; ++i) {
        g_read_scratch[i] = 0;
    }
    uint32_t off = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < k_max_files; ++i) {
        if (!g_files[i].used) {
            continue;
        }
        uint32_t name_len = 0;
        while (name_len < 32 && g_files[i].name[name_len] != '\0') {
            ++name_len;
        }
        if (off + name_len + 1 >= k_page_size) {
            break;
        }
        for (uint32_t k = 0; k < name_len; ++k) {
            g_read_scratch[off++] = static_cast<uint8_t>(g_files[i].name[k]);
        }
        g_read_scratch[off++] = '\0';
        ++count;
    }
    out.page_count = 1;
    out.pages[0].vaddr = reinterpret_cast<uint64_t>(g_read_scratch);
    out.pages[0].length = k_page_size;
    out.pages[0].mode = uapi::transfer_mode::copy;
    out.regs[0] = k_status_ok;
    out.regs[1] = count;
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
                case k_op_list:
                    handle_list(in, out);
                    break;
                default:
                    out.regs[1] = k_status_not_found;
                    break;
            }
        }
        do_syscall(uapi::k_syscall_ipc_reply, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
