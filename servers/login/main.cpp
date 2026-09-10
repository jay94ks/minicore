// servers/login/main.cpp — 로그인 프롬프트 프로세스
// (docs/plan/system-servers-bringup.md §M17, security-model.md
// ADR-165).
//
// console(TTY 출력)/ps2(키 입력)/procsrv(계정 인증) 세 서버 모두에
// 의존한다. 실제 키 입력이 QEMU 자동화 환경에 주입되지 않는 문제
// (ADR-130이 PS/2 자체 테스트로 이미 겪은 것과 같은 제약, ADR-165
// §결정4)는 같은 해법을 재사용한다 — 사용자명 프롬프트에서 정해진
// 횟수만큼 폴링해도 키가 하나도 안 오면(자동화 환경은 항상 이
// 경우다), 내장 자체 테스트 계정으로 **같은 OP_LOGIN 경로**를 그대로
// 호출해 인증 파이프라인 전체를 결정적으로 검증한다. 실제 사람이
// 타이핑하면 그 입력이 먼저 도착해 실제 경로를 그대로 탄다.
//
// 로그인 성공 이후의 session_program 스폰(ADR-089)은 이 라운드의
// 범위 밖이다 — VFS가 임의 경로의 ELF를 찾아 실행하는 일반 메커니즘이
// 아직 없다(system-servers-bringup.md §M20이 "로그인 후 셸"을 자신의
// 목표로 이미 명시해 뒀다).
#include <mc/syscall.h>

namespace kernsrv::login {

namespace {

// sys_process_spawn(create_endpoint=true)이 handle 1을 채운다 —
// login은 아무도 자신에게 걸지 않으므로 미사용. handle 2/3/4는
// `depends=console,ps2,procsrv` 나열 순서 그대로(init/initrun/main.cpp
// 의 콤마 분리 로직 참고).
constexpr uint32_t k_console_handle = 2;
constexpr uint32_t k_ps2_handle = 3;
constexpr uint32_t k_procsrv_handle = 4;

constexpr uint32_t k_console_op_print = 1;
constexpr uint32_t k_ps2_op_read_key = 1;
constexpr uint32_t k_procsrv_op_login = 4;
constexpr uint32_t k_procsrv_op_su = 5;  // security-model.md ADR-167.

constexpr uint8_t k_ascii_backspace = 0x08;
constexpr uint8_t k_ascii_enter = 0x0D;

// ps2 OP_READ_KEY 한 번의 폴링 예산은 드라이버 쪽에 있다(50,000
// 사이클 — 포트 I/O 트랩 하나하나가 QEMU에서 실제로 비싸서, 처음
// 2,000,000으로 뒀다가 50번 재시도(=1억 회 트랩)가 스모크 테스트
// 타임아웃을 실제로 넘긴 걸 겪고 줄였다). 여기서는 "몇 번이나 다시
// 물어볼까"만 정한다. 자동화 환경에서는 이 예산이 전부 소진돼도
// 키가 하나도 안 온다(결정적).
constexpr uint32_t k_max_consecutive_misses = 10;
constexpr uint32_t k_max_login_attempts = 3;
constexpr uint32_t k_max_line_len = 24;  // console OP_PRINT/procsrv OP_LOGIN 필드 폭에 맞춘 상한.

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
    do_syscall(MC_SYSCALL_DEBUG_LOG, reinterpret_cast<uint64_t>(msg), cstr_len(msg), 0);
}

void pack_bytes(void* dst, uint64_t dst_bytes, const char* data, uint64_t len) {
    auto* d = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < dst_bytes; ++i) {
        d[i] = (i < len) ? static_cast<uint8_t>(data[i]) : 0;
    }
}

// console에게 문자 하나(또는 짧은 문자열)를 보낸다 — fs-protocol류
// 서버들과 같은 정신(regs[]만으로 충분, servers/drivers/console/main.cpp
// 참고): regs[0]=길이, regs[1..3]=문자 바이트(최대 24바이트).
void console_print(const char* text, uint64_t len) {
    if (len > k_max_line_len) {
        len = k_max_line_len;
    }
    mc_message req{};
    req.label = k_console_op_print;
    req.regs[0] = len;
    pack_bytes(&req.regs[1], 3 * sizeof(uint64_t), text, len);
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_console_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
}
void console_print_str(const char* s) { console_print(s, cstr_len(s)); }
void console_print_char(char c) { console_print(&c, 1); }

struct read_line_result {
    uint32_t length = 0;
    bool got_any_key = false;
};

// echo=false면(비밀번호) 화면에 아무것도 보여주지 않는다 — 전통적인
// 유닉스 login 관례(별표 마스킹도 안 함, 아예 표시 안 함)와 같다.
read_line_result read_line(char* buf, uint32_t max_len, bool echo) {
    read_line_result result{};
    uint32_t consecutive_misses = 0;
    while (consecutive_misses < k_max_consecutive_misses) {
        mc_message req{};
        req.label = k_ps2_op_read_key;
        mc_message reply{};
        do_syscall(MC_SYSCALL_IPC_CALL, k_ps2_handle, reinterpret_cast<uint64_t>(&req),
                   reinterpret_cast<uint64_t>(&reply));
        if (reply.regs[0] == 0) {
            ++consecutive_misses;
            continue;
        }
        consecutive_misses = 0;
        result.got_any_key = true;
        auto ch = static_cast<uint8_t>(reply.regs[2]);
        if (ch == k_ascii_enter) {
            break;
        }
        if (ch == k_ascii_backspace) {
            if (result.length > 0) {
                --result.length;
                if (echo) {
                    console_print_char(k_ascii_backspace);
                }
            }
            continue;
        }
        if (result.length + 1 < max_len) {
            buf[result.length++] = static_cast<char>(ch);
            if (echo) {
                console_print_char(static_cast<char>(ch));
            }
        }
    }
    return result;
}

bool try_login(const char* username, uint64_t username_len, const char* password,
               uint64_t password_len) {
    mc_message req{};
    req.label = k_procsrv_op_login;
    pack_bytes(&req.regs[0], sizeof(uint64_t), username, username_len);
    pack_bytes(&req.regs[1], 2 * sizeof(uint64_t), password, password_len);
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_procsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    return reply.regs[0] == 0;
}

// security-model.md ADR-167 — OP_SU: regs[0]=호출자 사용자명(8바이트),
// regs[1]=대상 사용자명(8바이트), regs[2..3]=대상 비밀번호(16바이트,
// 위임이 없을 때만 검사). 응답 regs[0]: 0=위임 승인, 1=비밀번호
// 승인, 2=거부.
uint64_t try_su(const char* caller, const char* target, const char* target_password) {
    mc_message req{};
    req.label = k_procsrv_op_su;
    pack_bytes(&req.regs[0], sizeof(uint64_t), caller, cstr_len(caller));
    pack_bytes(&req.regs[1], sizeof(uint64_t), target, cstr_len(target));
    pack_bytes(&req.regs[2], 2 * sizeof(uint64_t), target_password, cstr_len(target_password));
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_procsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    return reply.regs[0];
}

// M18 — 정책 확인 경로 양쪽을 검증한다: (1) root가 test에게 위임해
// 뒀으므로 비밀번호 없이 승인돼야 한다(procsrv의 g_delegations),
// (2) guest1→test는 위임이 없고 잘못된 비밀번호를 주므로 거부돼야
// 한다.
void run_su_tests() {
    uint64_t delegated_status = try_su("test", "root", "");
    const char* m1 = (delegated_status == 0) ? "[login] su delegated ok=1\n"
                                                : "[login] su delegated ok=0\n";
    debug_log(m1);

    uint64_t denied_status = try_su("guest1", "test", "wrongpass");
    const char* m2 = (denied_status == 2) ? "[login] su denied ok=1\n" : "[login] su denied ok=0\n";
    debug_log(m2);
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    for (uint32_t attempt = 0; attempt < k_max_login_attempts; ++attempt) {
        console_print_str("minicore login: ");
        char username[k_max_line_len] = {};
        read_line_result user_result = read_line(username, sizeof(username), /*echo=*/true);

        bool login_ok;
        if (!user_result.got_any_key) {
            // 자동화 환경(ADR-165 §결정4) — 내장 자체 테스트 계정으로
            // 같은 OP_LOGIN 경로를 그대로 검증한다.
            debug_log("[login] no keyboard input, using self-test account\n");
            login_ok = try_login("test", 4, "test1234", 8);
        } else {
            console_print_str("\n");
            console_print_str("Password: ");
            char password[k_max_line_len] = {};
            read_line_result pass_result = read_line(password, sizeof(password), /*echo=*/false);
            console_print_str("\n");
            login_ok = try_login(username, user_result.length, password, pass_result.length);
        }

        if (login_ok) {
            console_print_str("Login successful\n");
            debug_log("[login] auth ok=1\n");
            // M18(security-model.md ADR-167) — 로그인 성공 이후
            // su/sudo 정책 확인 경로를 검증한다(위임 승인/거부 양쪽).
            run_su_tests();
            // M43(user-service-manager.md §M43, docs/design/
            // security-model.md ADR-218) — 계정별 유저 서비스
            // 인스턴스 검증은 계정이 최소 두 개 로그인해야 증명된다
            // (계획 원문의 검증 목표). 위의 자동 로그인은 "test"
            // 하나뿐이었으니 여기서 "root"로도 한 번 더 로그인해
            // procsrv의 로그인 이벤트 큐(handle_login::push_login_event)
            // 를 두 번째로 채운다.
            bool second_login_ok = try_login("root", 4, "root1234", 8);
            debug_log(second_login_ok ? "[login] second account (root) login ok=1\n"
                                       : "[login] second account (root) login ok=0\n");
            break;
        }
        console_print_str("Login incorrect\n");
        debug_log("[login] auth ok=0\n");
        if (attempt + 1 == k_max_login_attempts) {
            debug_log("[login] giving up after max attempts\n");
        }
    }

    // M20("로그인 후 셸")이 session_program 스폰을 맡는다 — 이
    // 라운드는 인증 성공/실패 판정까지만 증명한다(ADR-165 §결정3).
    do_syscall(MC_SYSCALL_THREAD_EXIT, 0, 0, 0);
    for (;;) {
        asm volatile("pause");
    }
}

}  // namespace kernsrv::login
