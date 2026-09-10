// servers/pipesrv/main.cpp — 익명 파이프 서버(docs/plan/
// musl-userland-porting.md §M51, mc/pipesrv_protocol.h). procsrv/
// cfgsrv/svcmgr와 같은 순수 minicore 네이티브 서버(libk+libmc만,
// musl 불필요) — 단일 요청-응답 루프이고, 절대 회신을 미루지
// 않는다(mc/pipesrv_protocol.h 상단 주석 참고 — 블로킹은 호출자
// (libc/sysdeps/minicore/syscall_shim.c)가 재시도로 흉내낸다).
//
// 파이프당 고정 4096바이트 원형 버퍼 하나(YAGNI, 계획 문서가 이미
// 정한 값). read_id/write_id는 이 서버가 내주는 불투명한 정수일
// 뿐 진짜 커널 핸들이 아니다 — dup()/fork()로 같은 id를 여러 곳이
// 들고 있을 수 있어 참조 카운트(read_refcount/write_refcount)로
// 관리한다. fork()는 이 서버 모르게 g_open_files류 지역 테이블을
// 그대로 복제하므로(syscall_shim.c의 SYS_fork 처리가 자식 쪽에서
// op_dup을 대신 불러 알려 준다), 이 서버는 자신에게 명시적으로
// 알려진 것만 참조 카운트에 반영한다.
#include <mc/pipesrv_protocol.h>
#include <mc/syscall.h>
#include <mc/util.h>

namespace {

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

constexpr uint32_t k_max_pipes = 8;

struct pipe_slot {
    bool used = false;
    uint8_t buf[MC_PIPE_CAPACITY] = {};
    uint32_t head = 0;  // 다음에 읽을 위치(원형).
    uint32_t len = 0;   // 현재 버퍼에 쌓인 바이트 수.
    uint32_t read_refcount = 0;
    uint32_t write_refcount = 0;
};

pipe_slot g_pipes[k_max_pipes];

// id = ((pipe_index << 1) | end_bit) + 1 — 0은 항상 무효라 +1로
// 밀어 둔다. end_bit: 0=read, 1=write.
uint64_t make_id(uint32_t index, bool is_write) {
    uint64_t raw = (static_cast<uint64_t>(index) << 1) | (is_write ? 1ull : 0ull);
    return raw + 1;
}

bool decode_id(uint64_t id, uint32_t& out_index, bool& out_is_write) {
    if (id == 0) {
        return false;
    }
    uint64_t raw = id - 1;
    out_index = static_cast<uint32_t>(raw >> 1);
    out_is_write = (raw & 1) != 0;
    return out_index < k_max_pipes && g_pipes[out_index].used;
}

void handle_create(mc_message& out) {
    for (uint32_t i = 0; i < k_max_pipes; ++i) {
        if (!g_pipes[i].used) {
            pipe_slot& p = g_pipes[i];
            p.used = true;
            p.head = 0;
            p.len = 0;
            p.read_refcount = 1;
            p.write_refcount = 1;
            out.regs[0] = MC_PIPE_STATUS_OK;
            out.regs[1] = make_id(i, false);
            out.regs[2] = make_id(i, true);
            return;
        }
    }
    out.regs[0] = MC_PIPE_STATUS_TABLE_FULL;
}

// procsrv/cfgsrv/vfs가 이미 쓰는 것과 같은 이유 — out.pages[0].vaddr
// 가 가리키는 메모리는 실제 sys_ipc_reply 시점까지 살아있어야 한다,
// 지역 배열을 쓰면 안 된다.
_Alignas(MC_PIPE_CAPACITY) uint8_t g_read_scratch[MC_PIPE_CAPACITY];

void handle_read(const mc_message& in, mc_message& out) {
    uint32_t index = 0;
    bool is_write = false;
    if (!decode_id(in.regs[0], index, is_write) || is_write) {
        out.regs[0] = MC_PIPE_STATUS_NOT_FOUND;
        return;
    }
    pipe_slot& p = g_pipes[index];
    if (p.len == 0) {
        if (p.write_refcount == 0) {
            // 진짜 EOF — WOULD_BLOCK이 아니라 OK+len=0(read()의 0
            // 반환과 같은 자리).
            out.regs[0] = MC_PIPE_STATUS_OK;
            out.regs[1] = 0;
            return;
        }
        out.regs[0] = MC_PIPE_STATUS_WOULD_BLOCK;
        return;
    }
    uint64_t requested_len = in.regs[1];
    uint64_t n = requested_len < p.len ? requested_len : p.len;
    if (n > MC_PIPE_CAPACITY) {
        n = MC_PIPE_CAPACITY;
    }
    mc_zero_bytes(g_read_scratch, sizeof(g_read_scratch));
    for (uint64_t i = 0; i < n; ++i) {
        g_read_scratch[i] = p.buf[(p.head + i) % MC_PIPE_CAPACITY];
    }
    p.head = static_cast<uint32_t>((p.head + n) % MC_PIPE_CAPACITY);
    p.len -= static_cast<uint32_t>(n);
    out.page_count = 1;
    out.pages[0].vaddr = reinterpret_cast<uint64_t>(g_read_scratch);
    out.pages[0].length = sizeof(g_read_scratch);
    out.pages[0].mode = MC_TRANSFER_COPY;
    out.regs[0] = MC_PIPE_STATUS_OK;
    out.regs[1] = n;
}

void handle_write(const mc_message& in, mc_message& out) {
    uint32_t index = 0;
    bool is_write = false;
    if (!decode_id(in.regs[0], index, is_write) || !is_write) {
        out.regs[0] = MC_PIPE_STATUS_NOT_FOUND;
        return;
    }
    pipe_slot& p = g_pipes[index];
    if (p.read_refcount == 0) {
        out.regs[0] = MC_PIPE_STATUS_BROKEN_PIPE;
        return;
    }
    uint32_t room = MC_PIPE_CAPACITY - p.len;
    if (room == 0) {
        out.regs[0] = MC_PIPE_STATUS_WOULD_BLOCK;
        return;
    }
    uint64_t len = in.regs[1];
    uint64_t n = len < room ? len : room;
    if (n > MC_PIPE_CAPACITY) {
        n = MC_PIPE_CAPACITY;
    }
    const uint8_t* src = reinterpret_cast<const uint8_t*>(in.pages[0].vaddr);
    uint32_t tail = (p.head + p.len) % MC_PIPE_CAPACITY;
    for (uint64_t i = 0; i < n; ++i) {
        p.buf[(tail + i) % MC_PIPE_CAPACITY] = src[i];
    }
    p.len += static_cast<uint32_t>(n);
    out.regs[0] = MC_PIPE_STATUS_OK;
    out.regs[1] = n;
}

void handle_close(const mc_message& in, mc_message& out) {
    uint32_t index = 0;
    bool is_write = false;
    if (!decode_id(in.regs[0], index, is_write)) {
        out.regs[0] = MC_PIPE_STATUS_NOT_FOUND;
        return;
    }
    pipe_slot& p = g_pipes[index];
    if (is_write) {
        if (p.write_refcount > 0) {
            --p.write_refcount;
        }
    } else {
        if (p.read_refcount > 0) {
            --p.read_refcount;
        }
    }
    if (p.read_refcount == 0 && p.write_refcount == 0) {
        p.used = false;
    }
    out.regs[0] = MC_PIPE_STATUS_OK;
}

void handle_dup(const mc_message& in, mc_message& out) {
    uint32_t index = 0;
    bool is_write = false;
    if (!decode_id(in.regs[0], index, is_write)) {
        out.regs[0] = MC_PIPE_STATUS_NOT_FOUND;
        return;
    }
    if (is_write) {
        ++g_pipes[index].write_refcount;
    } else {
        ++g_pipes[index].read_refcount;
    }
    out.regs[0] = MC_PIPE_STATUS_OK;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    constexpr uint32_t k_own_endpoint_handle = 1;
    for (;;) {
        mc_message in{};
        do_syscall(MC_SYSCALL_IPC_RECV, k_own_endpoint_handle, reinterpret_cast<uint64_t>(&in), 0);
        mc_message out{};
        out.label = in.label;
        switch (in.label) {
            case MC_PIPE_OP_CREATE:
                handle_create(out);
                break;
            case MC_PIPE_OP_READ:
                handle_read(in, out);
                break;
            case MC_PIPE_OP_WRITE:
                handle_write(in, out);
                break;
            case MC_PIPE_OP_CLOSE:
                handle_close(in, out);
                break;
            case MC_PIPE_OP_DUP:
                handle_dup(in, out);
                break;
            default:
                break;
        }
        do_syscall(MC_SYSCALL_IPC_REPLY, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
