// servers/drivers/console/main.cpp — VGA 텍스트 콘솔 드라이버
// (docs/plan/system-servers-bringup.md §M17, boot-and-drivers.md
// ADR-097/111/164).
//
// M1의 디버그 시리얼 콘솔(debug-console.md)과는 완전히 별개다 — 여기서
// 처음으로 진짜 "유저에게 보이는" 콘솔이 생긴다. ADR-164가 확정한
// M17 범위: TTY 1개만 하드코딩(cfgsrv가 아직 없다, M19 이후로 멀티
// TTY 미룸), 렌더링 백엔드는 VGA 텍스트 모드(물리주소 0xB8000,
// 80x25, 셀당 2바이트: 문자+속성) 하나뿐 — 그래픽 프레임버퍼 백엔드는
// 그래픽 드라이버가 실제로 생기는 시점까지 범위 밖. `sys_map_phys`
// (ADR-156)로 매핑한다 — PCI 장치가 아니라 QEMU q35의 레거시 VGA
// 호환 영역에 직접 접근하므로 devmgr 등록이 필요 없다(trusted=1만
// 필요).
#include <uapi.hpp>

namespace {

constexpr uint32_t k_own_endpoint_handle = 1;

constexpr uint32_t k_op_print = 1;
constexpr uint64_t k_status_ok = 0;

constexpr uint64_t k_vga_phys = 0x00000000000B8000ull;
constexpr uint32_t k_cols = 80;
constexpr uint32_t k_rows = 25;
constexpr uint8_t k_default_attr = 0x07;  // 밝은 회색 글자, 검은 배경.
constexpr uint8_t k_ascii_backspace = 0x08;

uint16_t* g_vga = nullptr;
uint32_t g_cursor_row = 0;
uint32_t g_cursor_col = 0;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

uint64_t cstr_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}
void debug_log(const char* msg) {
    do_syscall(uapi::k_syscall_debug_log, reinterpret_cast<uint64_t>(msg), cstr_len(msg), 0);
}

// 한 줄을 위로 밀어 올린다(마지막 줄이 25번째를 넘어갈 때) — 문자
// 셀을 통째로 옮기고 마지막 줄은 공백으로 지운다.
void scroll_up() {
    for (uint32_t row = 1; row < k_rows; ++row) {
        for (uint32_t col = 0; col < k_cols; ++col) {
            g_vga[(row - 1) * k_cols + col] = g_vga[row * k_cols + col];
        }
    }
    for (uint32_t col = 0; col < k_cols; ++col) {
        g_vga[(k_rows - 1) * k_cols + col] = static_cast<uint16_t>(' ') | (k_default_attr << 8);
    }
}

void newline() {
    g_cursor_col = 0;
    ++g_cursor_row;
    if (g_cursor_row >= k_rows) {
        scroll_up();
        g_cursor_row = k_rows - 1;
    }
}

// fs-protocol류 서버들과 같은 정신(regs[]만으로 충분히 짧은
// 프롬프트/메시지를 옮긴다, ADR-164 §결정3) — 한 바이트씩 VGA
// 버퍼에 쓴다. '\n'은 다음 줄, 0x08(backspace)은 커서를 한 칸
// 물리고 그 자리를 지운다(로그인 프롬프트가 사용자 입력을 지울 때
// 그대로 보낸다).
void print_char(char c) {
    if (c == '\n') {
        newline();
        return;
    }
    if (static_cast<uint8_t>(c) == k_ascii_backspace) {
        if (g_cursor_col > 0) {
            --g_cursor_col;
            g_vga[g_cursor_row * k_cols + g_cursor_col] =
                static_cast<uint16_t>(' ') | (k_default_attr << 8);
        }
        return;
    }
    g_vga[g_cursor_row * k_cols + g_cursor_col] =
        static_cast<uint16_t>(static_cast<uint8_t>(c)) | (k_default_attr << 8);
    ++g_cursor_col;
    if (g_cursor_col >= k_cols) {
        newline();
    }
}

void handle_print(const uapi::message& in, uapi::message& out) {
    uint64_t length = in.regs[0];
    if (length > 3 * sizeof(uint64_t)) {
        length = 3 * sizeof(uint64_t);  // regs[1..3] = 24바이트 상한.
    }
    const auto* text_bytes = reinterpret_cast<const char*>(&in.regs[1]);
    for (uint64_t i = 0; i < length; ++i) {
        print_char(text_bytes[i]);
    }
    out.regs[1] = k_status_ok;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    uapi::map_phys_request req{};
    req.phys_addr = k_vga_phys;
    req.size = k_cols * k_rows * 2;
    uint64_t err = do_syscall(uapi::k_syscall_map_phys, reinterpret_cast<uint64_t>(&req), 0, 0);
    if (err != 0) {
        debug_log("[console] map_phys failed\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    g_vga = reinterpret_cast<uint16_t*>(req.out_virt_addr);

    // 화면을 지우고 왼쪽 위로 커서를 둔다 — 부팅 로그(디버그 시리얼
    // 콘솔)와 뒤섞이지 않는 깨끗한 시작.
    for (uint32_t i = 0; i < k_cols * k_rows; ++i) {
        g_vga[i] = static_cast<uint16_t>(' ') | (k_default_attr << 8);
    }
    g_cursor_row = 0;
    g_cursor_col = 0;

    debug_log("[console] vga init ok=1\n");

    for (;;) {
        uapi::message in{};
        uint64_t recv_err = do_syscall(uapi::k_syscall_ipc_recv, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in), 0);
        uapi::message out{};
        if (recv_err == 0) {
            out.label = in.label;
            if (in.label == k_op_print) {
                handle_print(in, out);
            }
        }
        do_syscall(uapi::k_syscall_ipc_reply, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
