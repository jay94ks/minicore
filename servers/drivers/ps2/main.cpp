// servers/drivers/ps2/main.cpp — PS/2 컨트롤러(8042) 드라이버
// (docs/plan/system-servers-bringup.md §M14, ADR-130 — devmgr 열거를
// 거치지 않는 고정 레거시 프로브).
//
// M14 최소 버전 — 실제 키 입력이 QEMU 자동화 환경에서는 주입되지
// 않으므로(스모크 테스트가 키를 누르지 않는다), 사용자 입력에
// 의존하지 않는 **컨트롤러 자체 진단(self-test)** 핸드셰이크로
// 검증 가능한 결과를 만든다 — 8042 컨트롤러 커맨드 0xAA(컨트롤러
// 자체 테스트)를 보내면 QEMU의 PS/2 에뮬레이션이 항상 0x55(통과)를
// 돌려준다. 그 뒤 짧게 출력 버퍼를 폴링해 실제로 키 입력이 있으면
// 스캔코드를 로그로 남긴다(있으면 보이고, 없어도 self-test 결과로
// 실제 포트 I/O가 됐음을 확인할 수 있다).
#include <uapi.hpp>

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

    do_syscall(uapi::k_syscall_io_deactivate, 0, 0, 0);
    do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    for (;;) {
        asm volatile("pause");
    }
}
