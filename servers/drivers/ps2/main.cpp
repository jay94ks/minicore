// servers/drivers/ps2/main.cpp — PS/2 컨트롤러(8042) 드라이버
// (docs/plan/system-servers-bringup.md §M14/§M17, ADR-130 — devmgr
// 열거를 거치지 않는 고정 레거시 프로브).
//
// M14는 실제 키 입력이 QEMU 자동화 환경에서는 주입되지 않으므로
// (스모크 테스트가 키를 누르지 않는다), 사용자 입력에 의존하지 않는
// **컨트롤러 자체 진단(self-test)** 핸드셰이크로 검증 가능한
// 결과를 만들었다 — 커맨드 0xAA(컨트롤러 자체 테스트)를 보내면
// QEMU의 PS/2 에뮬레이션이 항상 0x55(통과)를 돌려준다. M17이 여기에
// 실제 스캔코드→ASCII 번역(`OP_READ_KEY`)을 추가하고, 이 드라이버를
// (self-test 후 바로 종료하던 것에서) `servers/login`이 호출할 수
// 있는 진짜 IPC 서버로 바꾼다 — 실제 키 입력이 없으면(자동화 환경)
// `OP_READ_KEY`가 그냥 "없음"을 반환할 뿐이라, self-test 결과와
// 마찬가지로 결정적이다(servers/login의 자체 테스트 폴백이 이걸
// 받아 처리한다, security-model.md ADR-165 §결정4).
#include <uapi.hpp>

namespace kernsrv::drivers::ps2 {

namespace {

constexpr uint16_t k_port_data = 0x60;
constexpr uint16_t k_port_status_cmd = 0x64;
constexpr uint8_t k_status_output_full = 1u << 0;
constexpr uint8_t k_cmd_self_test = 0xAA;
constexpr uint8_t k_self_test_pass = 0x55;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

uint8_t in8(uint16_t port) {
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
void out8(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }

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
void debug_log_hex(const char* prefix, uint64_t value) {
    char buf[96];
    uint64_t i = 0;
    for (; prefix[i] != '\0' && i < 60; ++i) {
        buf[i] = prefix[i];
    }
    buf[i++] = '0';
    buf[i++] = 'x';
    bool leading = true;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0xF);
        if (nibble != 0 || !leading || shift == 0) {
            leading = false;
            buf[i++] = static_cast<char>(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10));
        }
    }
    buf[i++] = '\n';
    do_syscall(uapi::k_syscall_debug_log, reinterpret_cast<uint64_t>(buf), i, 0);
}

// 출력 버퍼가 찰 때까지 유한 횟수만 기다린다(ADR-131 §근거와 같은
// 정신 — 인터럽트 없는 순수 폴링, 부트스트랩 성격의 드라이버).
bool wait_output_full(uint32_t max_iterations) {
    for (uint32_t i = 0; i < max_iterations; ++i) {
        if ((in8(k_port_status_cmd) & k_status_output_full) != 0) {
            return true;
        }
    }
    return false;
}

// ---------- M17: OP_READ_KEY(security-model.md ADR-165) ----------
constexpr uint32_t k_own_endpoint_handle = 1;
constexpr uint32_t k_op_read_key = 1;
constexpr uint64_t k_status_ok = 0;

// 한 번의 IPC 호출당 폴링 예산 — 실제 키 입력이 QEMU 자동화 환경에
// 주입되지 않으므로(이 파일 상단 주석), 이 예산은 항상 다 소진되고
// "없음"으로 응답한다. 사람이 QEMU 창에서 실제로 타이핑하면 이
// 예산 안에 도착해 그대로 처리된다.
constexpr uint32_t k_read_key_poll_budget = 50'000;

constexpr uint8_t k_scancode_backspace = 0x0E;
constexpr uint8_t k_scancode_enter = 0x1C;
constexpr uint8_t k_ascii_backspace = 0x08;
constexpr uint8_t k_ascii_enter = 0x0D;

// scancode set 1(US QWERTY) make code → ASCII. Shift/Caps는 다루지
// 않는다(로그인 사용자명/비밀번호가 소문자만 필요한 M17 범위,
// security-model.md ADR-165 근거) — 매핑 없는 코드는 0.
uint8_t scancode_to_ascii(uint8_t code) {
    static constexpr uint8_t k_table[] = {
        /*0x00*/ 0,    0,    '1', '2', '3', '4', '5', '6',
        /*0x08*/ '7',  '8',  '9', '0', '-', '=', 0 /*BS*/, 0,
        /*0x10*/ 'q',  'w',  'e', 'r', 't', 'y', 'u', 'i',
        /*0x18*/ 'o',  'p',  '[', ']', 0 /*Enter*/, 0, 'a', 's',
        /*0x20*/ 'd',  'f',  'g', 'h', 'j', 'k', 'l', ';',
        /*0x28*/ '\'', '`',  0,   '\\', 'z', 'x', 'c', 'v',
        /*0x30*/ 'b',  'n',  'm', ',', '.', '/', 0,   '*',
        /*0x38*/ 0,    ' ',
    };
    if (code >= sizeof(k_table)) {
        return 0;
    }
    return k_table[code];
}

// 결정적 예산 안에서 키 하나를 읽어 본다. brk 코드(0x80 이상, 키를
// 뗄 때 나오는 코드)는 무시한다 — make 코드만 다룬다.
void handle_read_key(uapi::message& out) {
    if (!wait_output_full(k_read_key_poll_budget)) {
        out.regs[0] = 0;  // 없음.
        out.regs[1] = k_status_ok;
        return;
    }
    uint8_t scancode = in8(k_port_data);
    if (scancode & 0x80) {
        out.regs[0] = 0;  // break 코드는 무시.
        out.regs[1] = k_status_ok;
        return;
    }
    uint8_t ascii = 0;
    if (scancode == k_scancode_backspace) {
        ascii = k_ascii_backspace;
    } else if (scancode == k_scancode_enter) {
        ascii = k_ascii_enter;
    } else {
        ascii = scancode_to_ascii(scancode);
    }
    out.regs[0] = (ascii != 0) ? 1 : 0;
    out.regs[1] = k_status_ok;
    out.regs[2] = ascii;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    // ADR-154 — 이 스레드 자신이 필요한 순간에 직접 활성화한다(포트
    // 0x60~0x64, 5개 — 데이터/상태/커맨드 레지스터를 넉넉히 덮는다).
    do_syscall(uapi::k_syscall_io_activate, k_port_data, 5, 0);

    // 컨트롤러 자체 테스트 — 사용자 입력과 무관하게 항상 결정적인
    // 결과를 준다(이 파일 상단 주석).
    out8(k_port_status_cmd, k_cmd_self_test);
    if (wait_output_full(1'000'000)) {
        uint8_t result = in8(k_port_data);
        debug_log_hex("[ps2] self-test result=", result);
        if (result == k_self_test_pass) {
            debug_log("[ps2] controller self-test ok=1\n");
        } else {
            debug_log("[ps2] controller self-test ok=0\n");
        }
    } else {
        debug_log("[ps2] controller self-test timeout\n");
    }

    // 실제 키 입력이 있으면(사람이 QEMU 창에 타이핑하는 경우 등)
    // 짧게 몇 개까지 스캔코드를 로그로 남긴다 — 없어도 위 self-test
    // 결과로 이미 실제 포트 I/O를 확인했으므로 실패로 취급하지 않는다.
    for (int i = 0; i < 8; ++i) {
        if (!wait_output_full(200'000)) {
            break;
        }
        uint8_t scancode = in8(k_port_data);
        debug_log_hex("[ps2] scancode=", scancode);
    }

    // M17(security-model.md ADR-165) — self-test가 끝난 뒤에도 종료
    // 하지 않는다. servers/login이 OP_READ_KEY로 실제 키보드 입력을
    // 요청할 수 있는 진짜 IPC 서버가 된다 — sys_io_activate는 계속
    // 유지한다(이 스레드가 계속 포트에 접근해야 하므로 io_deactivate
    // 호출을 없앴다).
    for (;;) {
        uapi::message in{};
        uint64_t recv_err = do_syscall(uapi::k_syscall_ipc_recv, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in), 0);
        uapi::message out{};
        if (recv_err == 0) {
            out.label = in.label;
            if (in.label == k_op_read_key) {
                handle_read_key(out);
            }
        }
        do_syscall(uapi::k_syscall_ipc_reply, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}

}  // namespace kernsrv::drivers::ps2
