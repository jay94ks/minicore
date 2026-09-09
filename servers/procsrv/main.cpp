// servers/procsrv/main.cpp — 프로세스 서버의 M12 골격
// (docs/plan/system-servers-bringup.md §M12, docs/spec/procsrv.md).
//
// M12는 procsrv.md가 정의하는 전체 프로세스 테이블·fd 진실 공급원·
// 계정 모델을 구현하지 않는다(계획 §M12 §구현2가 이미 "프로토콜
// 골격만"으로 scope했다) — 이 마일스톤의 QEMU 검증 목표는 딱 하나,
// "procsrv가 실제 디스크(virtio-blk+cpio)에서 읽혀 sys_process_spawn
// 으로 기동된 뒤, 자기 자신을 fork/exec해 실제 두 번째 완전한 유저
// 프로세스를 만들어내는 것"이다(kernel_main.cpp가 initrun에 대해
// 이미 검증한 것과 같은 절차를 procsrv 자신에게 반복하는 것 —
// ADR-149가 그 self_info 배선을 procsrv도 받을 수 있도록 일반화해
// 둬서 가능해졌다).
//
// initrun과 다른 점 — "나는 initrun이 방금 스폰한 원본인가, 아니면
// fork/exec으로 만들어진 사본인가"를 구분할 방법이 initrun의
// boot_info(항상 비어있지 않은 포인터)와 다르다: procsrv는 일반
// sys_process_spawn으로 만들어지므로 RDI는 그냥 argv 블록 주소(또는
// 인자가 없으면 0)다. 그래서 이 규약을 쓴다 — **initrun이 procsrv를
// 처음 스폰할 때만 argv를 비워두지 않고(예: "-" 한 글자짜리 블록),
// procsrv 자신의 self-exec 호출은 항상 argv 없이(0) 한다** — initrun의
// boot_info-vs-null 구분과 정확히 같은 비대칭을 argv 유무로 재현한다.
//
// M13(system-servers-bringup.md §M13, docs/spec/fs-protocol.md) — 여기
// procsrv를 "VFS를 실제로 쓰는 M12의 두 번째 프로세스"로 그대로
// 재사용한다(계획 §M13 §목표가 명시한 표현 그대로) — vfs에 파일을
// 열고 쓰고 다시 읽어 내용이 일치하는지 확인한다. procsrv.md의 실제
// 프로토콜(§2~9)은 여전히 구현하지 않는다 — 이 라운드트립은 그것과
// 무관한, fs-protocol.md 클라이언트 역할의 최소 검증일 뿐이다.
//
// M18(system-servers-bringup.md §M18, security-model.md ADR-167) —
// su/sudo 검증을 위해 "경로→ELF 로더"를 실제로 구현한다: procsrv가
// 자기 자신의 ELF 바이트(M12 self_info 브릿지로 이미 갖고 있다)를
// VFS 경로(`/bin/su-target`)에 실제로 쓰고, 다시 그 경로에서 읽어
// 재조립한 바이트가 원본과 정확히 일치함을 확인한 뒤, 그 재조립된
// 바이트로 `sys_process_spawn`한다 — 스폰되는 프로세스는 procsrv와
// 같은 바이너리이지만 magic 접두사가 붙은 argv(`su_target_argv`)로
// "이번엔 su-target 역할을 하라"고 구분해 받는다(procsrv 자신의
// self-exec 판별 관례를 확장한 것, 이 파일 상단 argv 규약 참고).
#include <uapi.hpp>

namespace {

// sys_process_spawn(create_endpoint=true)이 handle 1을 이 프로세스의
// endpoint로 채운다(ADR-152) — M17부터 servers/login이 OP_LOGIN으로
// 이 handle에 건다(security-model.md ADR-165). handle 2는 initrun이
// 스폰 시점에 넣어 준 vfs endpoint 프록시(lib/*.ini의 `depends=vfs`).
constexpr uint32_t k_own_endpoint_handle = 1;
constexpr uint32_t k_vfs_handle = 2;

constexpr uint32_t k_op_open = 1;
constexpr uint32_t k_op_write = 2;
constexpr uint32_t k_op_read = 3;
constexpr uint32_t k_path_budget = 3 * sizeof(uint64_t);  // M18(fs-protocol.md v3) — regs[0..2]=24바이트, regs[3]=신원.

// M17(security-model.md ADR-165) — procsrv가 이번 라운드에서 처음
// 서버가 되어 받는 오퍼레이션. OP_LOGIN: regs[0]=사용자명(최대
// 8바이트, NUL 패딩), regs[1..2]=비밀번호(최대 16바이트, NUL 패딩),
// 응답 regs[0]=상태(0=성공, 1=실패). ADR-079의 uid/gid/S/G/J 비트는
// 아직 채우지 않는다 — "계정이 존재하고 평문 비밀번호가 일치하면
// 성공"만 증명하는 최소 저장소다.
constexpr uint32_t k_op_login = 4;
constexpr uint64_t k_login_status_ok = 0;
constexpr uint64_t k_login_status_denied = 1;

// M18(security-model.md ADR-167) — OP_SU: regs[0]=호출자 사용자명
// (8바이트), regs[1]=대상 사용자명(8바이트), regs[2..3]=대상
// 비밀번호(16바이트, 위임이 없을 때만 검사). 응답 regs[0]=상태
// (0=위임으로 승인, 1=비밀번호로 승인, 2=거부), regs[1]=대상 uid.
constexpr uint32_t k_op_su = 5;
constexpr uint64_t k_su_status_granted_by_delegation = 0;
constexpr uint64_t k_su_status_granted_by_password = 1;
constexpr uint64_t k_su_status_denied = 2;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

[[noreturn]] void quiet_exit() {
    do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    for (;;) {
        asm volatile("pause");
    }
}

void debug_log(const char* msg, uint64_t len) {
    do_syscall(uapi::k_syscall_debug_log, reinterpret_cast<uint64_t>(msg), len, 0);
}

uint64_t cstr_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

// fs-protocol.md §1 — M13은 pages[]를 안 쓰고 regs[]에 짧은 바이트를
// 그대로 눌러 담는다. path는 regs[0..3](32바이트) 전체, 데이터는
// regs[2..3](16바이트)만 — dst_bytes로 정확히 그 폭만 채운다(그
// 이상/이하로 쓰면 이웃 필드를 침범하거나 남은 바이트가 안 지워진다).
void pack_bytes(void* dst, uint64_t dst_bytes, const char* data, uint64_t len) {
    auto* d = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < dst_bytes; ++i) {
        d[i] = (i < len) ? static_cast<uint8_t>(data[i]) : 0;
    }
}

bool bytes_equal(const void* a, const void* b, uint64_t len) {
    const auto* pa = static_cast<const uint8_t*>(a);
    const auto* pb = static_cast<const uint8_t*>(b);
    for (uint64_t i = 0; i < len; ++i) {
        if (pa[i] != pb[i]) {
            return false;
        }
    }
    return true;
}

// M18(security-model.md ADR-079 최소 버전/ADR-167) — uid/S·G·J
// 비트를 이제 채운다. 여전히 하드코딩이다(ADR-165 §결정2와 같은
// 이유) — 실제 uid 채번·해싱은 M18 범위 밖.
struct account {
    char username[8];
    char password[16];
    uint32_t uid;
    bool is_super;
    bool is_guest;
    bool is_jail;
};

constexpr uint32_t k_num_accounts = 3;
account g_accounts[k_num_accounts];

void init_accounts() {
    const char* names[k_num_accounts] = {"test", "root", "guest1"};
    const char* passwords[k_num_accounts] = {"test1234", "root1234", "guest1234"};
    const uint32_t uids[k_num_accounts] = {1000, 0, 2000};
    const bool supers[k_num_accounts] = {false, true, false};
    const bool guests[k_num_accounts] = {false, false, true};
    for (uint32_t a = 0; a < k_num_accounts; ++a) {
        pack_bytes(g_accounts[a].username, sizeof(g_accounts[a].username), names[a],
                   cstr_len(names[a]));
        pack_bytes(g_accounts[a].password, sizeof(g_accounts[a].password), passwords[a],
                   cstr_len(passwords[a]));
        g_accounts[a].uid = uids[a];
        g_accounts[a].is_super = supers[a];
        g_accounts[a].is_guest = guests[a];
        g_accounts[a].is_jail = false;  // M18은 jail을 guest와 같은 규칙으로 다룬다(ADR-167 §결정2) — 별도 계정 불필요.
    }
}

void handle_login(const uapi::message& in, uapi::message& out) {
    for (const account& acc : g_accounts) {
        if (bytes_equal(&in.regs[0], acc.username, sizeof(acc.username)) &&
            bytes_equal(&in.regs[1], acc.password, sizeof(acc.password))) {
            out.regs[0] = k_login_status_ok;
            return;
        }
    }
    out.regs[0] = k_login_status_denied;
}

// M18(security-model.md ADR-093/167) — "@global/system/delegates/<계정>"
// 레지스트리 테이블(cfgsrv, M19)의 하드코딩 대체. {target_uid,
// allowed_caller_uid} — root(uid=0)가 test(uid=1000)에게 위임해
// 뒀다: test는 root의 비밀번호 없이 su할 수 있다.
struct delegation {
    uint32_t target_uid;
    uint32_t allowed_caller_uid;
};
constexpr delegation g_delegations[] = {
    {0, 1000},
};

bool is_delegated(uint32_t target_uid, uint32_t caller_uid) {
    for (const delegation& d : g_delegations) {
        if (d.target_uid == target_uid && d.allowed_caller_uid == caller_uid) {
            return true;
        }
    }
    return false;
}

const account* find_account_by_username(const void* username_bytes) {
    for (const account& acc : g_accounts) {
        if (bytes_equal(username_bytes, acc.username, sizeof(acc.username))) {
            return &acc;
        }
    }
    return nullptr;
}

// ---------- M18 — 경로→ELF 로더 + su-target 스폰(security-model.md ADR-167) ----------
constexpr uint64_t k_page_size = 4096;
constexpr uint64_t k_max_reassembled_bytes = 131072;  // servers/fs/memfs::k_max_file_bytes와 일치.
constexpr const char* k_su_target_path = "/bin/su-target";

alignas(k_page_size) uint8_t g_write_scratch[k_page_size] = {};
uint8_t g_reassembled[k_max_reassembled_bytes] = {};
uint64_t g_reassembled_size = 0;
bool g_loader_ok = false;

// magic 접두사가 붙은 argv — procsrv 자신을 su-target 역할로 다시
// 스폰할 때 쓴다(이 파일 상단 주석). 일반 부트 스폰(마커 1바이트)·
// self-exec 사본(argv=nullptr)과는 크기/내용으로 구분된다.
constexpr uint32_t k_su_target_magic = 0x53555354;
struct su_target_argv {
    uint32_t magic = 0;
    uint32_t uid = 0;
    uint8_t is_super = 0;
    uint8_t is_guest = 0;
    uint8_t is_jail = 0;
    uint8_t reserved = 0;
};

bool is_su_target_argv(const void* argv) {
    if (argv == nullptr) {
        return false;
    }
    uint32_t magic;
    __builtin_memcpy(&magic, argv, sizeof(magic));
    return magic == k_su_target_magic;
}

// vfs에 path를 열어 open_file_id/fs_handle을 얻는다(identity=0 —
// 로더 자신은 guest/jail이 아니다). 실패하면 fs_handle=0.
void vfs_open(const char* path, uint64_t& out_open_file_id, uint32_t& out_fs_handle) {
    uapi::message req{};
    req.label = k_op_open;
    pack_bytes(req.regs, k_path_budget, path, cstr_len(path));
    req.regs[3] = 0;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_vfs_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    if (reply.regs[1] != 0 || reply.handle_count != 1) {
        out_fs_handle = 0;
        return;
    }
    out_open_file_id = reply.regs[0];
    out_fs_handle = reply.handles[0].src_handle;
}

// path에 data[0..size)를 페이지 단위로 나눠 쓴다(fs-protocol.md v3
// §2.2) — memfs의 write_cursor가 자동으로 이어 쓴다.
bool write_elf_to_vfs(const char* path, const uint8_t* data, uint64_t size) {
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    vfs_open(path, open_file_id, fs_handle);
    if (fs_handle == 0) {
        return false;
    }
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t chunk = size - offset;
        if (chunk > k_page_size) {
            chunk = k_page_size;
        }
        for (uint64_t i = 0; i < k_page_size; ++i) {
            g_write_scratch[i] = (i < chunk) ? data[offset + i] : 0;
        }
        uapi::message req{};
        req.label = k_op_write;
        req.regs[0] = open_file_id;
        req.regs[1] = chunk;
        req.page_count = 1;
        req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_write_scratch);
        req.pages[0].length = k_page_size;
        req.pages[0].mode = uapi::transfer_mode::copy;
        uapi::message reply{};
        do_syscall(uapi::k_syscall_ipc_call, fs_handle, reinterpret_cast<uint64_t>(&req),
                   reinterpret_cast<uint64_t>(&reply));
        if (reply.regs[1] != 0 || reply.regs[0] != chunk) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

// path를 열어 EOF까지 순차적으로 읽어 out_buf에 재조립한다
// (fs-protocol.md v3 §2.3) — memfs의 read_cursor가 자동으로 이어
// 읽는다. 반환값은 실제로 읽은 총 바이트 수.
uint64_t read_elf_from_vfs(const char* path, uint8_t* out_buf, uint64_t max_len) {
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    vfs_open(path, open_file_id, fs_handle);
    if (fs_handle == 0) {
        return 0;
    }
    uint64_t total = 0;
    for (;;) {
        uapi::message req{};
        req.label = k_op_read;
        req.regs[0] = open_file_id;
        req.regs[1] = k_page_size;
        uapi::message reply{};
        do_syscall(uapi::k_syscall_ipc_call, fs_handle, reinterpret_cast<uint64_t>(&req),
                   reinterpret_cast<uint64_t>(&reply));
        if (reply.regs[1] != 0 || reply.page_count != 1) {
            break;
        }
        uint64_t n = reply.regs[0];
        if (n == 0) {
            break;  // EOF.
        }
        if (total + n > max_len) {
            n = max_len - total;
        }
        const auto* src = reinterpret_cast<const uint8_t*>(reply.pages[0].vaddr);
        for (uint64_t i = 0; i < n; ++i) {
            out_buf[total + i] = src[i];
        }
        total += n;
        if (n < k_page_size || total >= max_len) {
            break;
        }
    }
    return total;
}

// M12 self_info 브릿지의 ELF 바이트를 /bin/su-target에 실제로 쓰고
// 다시 읽어 재조립한 뒤 원본과 정확히 일치하는지 확인한다 — 이후
// su/sudo가 스폰하는 바이트는 이 재조립된 버퍼다(원본을 직접 쓰는
// 게 아니라, "경로로 저장하고 다시 읽어 실행"이라는 로더의 실제
// 파이프라인을 그대로 타게 한다).
void run_loader_test(const uapi::m12_self_info& self_info) {
    const auto* original = reinterpret_cast<const uint8_t*>(self_info.elf_addr);
    bool write_ok = write_elf_to_vfs(k_su_target_path, original, self_info.elf_size);
    g_reassembled_size = write_ok ? read_elf_from_vfs(k_su_target_path, g_reassembled,
                                                       sizeof(g_reassembled))
                                   : 0;
    g_loader_ok = write_ok && g_reassembled_size == self_info.elf_size &&
                  bytes_equal(g_reassembled, original, self_info.elf_size);
    const char* msg = g_loader_ok ? "[procsrv] loader roundtrip ok=1\n"
                                    : "[procsrv] loader roundtrip ok=0\n";
    debug_log(msg, cstr_len(msg));
}

// 재조립된 바이트(로더로 검증된)를 su-target argv로 다시 스폰한다 —
// 스폰되는 프로세스는 procsrv와 같은 코드이지만 magic argv로 다른
// 역할(run_as_su_target)을 탄다. vfs 핸들을 상속시켜 guest 격리
// 테스트가 그걸로 직접 open을 시도할 수 있게 한다.
void spawn_su_target(uint32_t uid, bool is_super, bool is_guest, bool is_jail) {
    if (!g_loader_ok) {
        return;
    }
    su_target_argv argv{};
    argv.magic = k_su_target_magic;
    argv.uid = uid;
    argv.is_super = is_super ? 1 : 0;
    argv.is_guest = is_guest ? 1 : 0;
    argv.is_jail = is_jail ? 1 : 0;

    uapi::process_spawn_request req{};
    req.elf_data = reinterpret_cast<uint64_t>(g_reassembled);
    req.elf_size = g_reassembled_size;
    req.argv_blob = reinterpret_cast<uint64_t>(&argv);
    req.argv_size = sizeof(argv);
    // create_endpoint=true를 유지한다 — handle 1(새 endpoint, 이
    // su-target 역할에서는 안 쓰지만)이 먼저 차야 inherited_handles[0]
    // 이 handle 2가 돼서 run_as_su_target()이 쓰는 k_vfs_handle(=2)
    // 상수와 일치한다(ADR-152의 고정 순서).
    req.create_endpoint = true;
    req.inherited_handle_count = 1;
    req.inherited_handles[0].src_handle = k_vfs_handle;
    req.inherited_handles[0].rights_mask = uapi::k_right_can_send;
    do_syscall(uapi::k_syscall_process_spawn, reinterpret_cast<uint64_t>(&req), 0, 0);
}

void handle_su(const uapi::message& in, uapi::message& out) {
    const account* caller = find_account_by_username(&in.regs[0]);
    const account* target = find_account_by_username(&in.regs[1]);
    if (caller == nullptr || target == nullptr) {
        out.regs[0] = k_su_status_denied;
        return;
    }
    // ADR-093 §결정7 — 대상이 guest/jail이면 위임을 무시하고 항상
    // 비밀번호를 요구한다.
    bool delegated = !target->is_guest && !target->is_jail &&
                      is_delegated(target->uid, caller->uid);
    bool granted;
    uint64_t status;
    if (delegated) {
        granted = true;
        status = k_su_status_granted_by_delegation;
    } else {
        bool password_ok = bytes_equal(&in.regs[2], target->password, sizeof(target->password));
        granted = password_ok;
        status = password_ok ? k_su_status_granted_by_password : k_su_status_denied;
    }
    out.regs[0] = status;
    out.regs[1] = target->uid;
    if (granted) {
        spawn_su_target(target->uid, target->is_super, target->is_guest, target->is_jail);
    }
}

// guest1 신원으로 su-target을 직접 스폰해(OP_SU를 거치지 않는다 —
// 이건 "누가 누구로 전환하는가"가 아니라 "guest 프로세스가 VFS
// 격리를 실제로 받는가"를 확인하는 별개의 검증이다) VFS 격리를
// 검증한다. su-target 자신이 홈 밖/안 open을 시도하고 결과를
// 로그로 남긴다(run_as_su_target 참고).
void run_guest_confinement_test() {
    spawn_su_target(2000, /*is_super=*/false, /*is_guest=*/true, /*is_jail=*/false);
}

// su_target_argv로 다시 스폰된 procsrv 사본의 역할. 신원을 로그로
// 남기고, guest/jail이면 홈 밖(거부 기대)/홈 안(성공 기대) open을
// 둘 다 시도해 결과를 로그로 남긴다 — handle 2는 spawn_su_target이
// 물려준 vfs 프록시다.
[[noreturn]] void run_as_su_target(const void* argv) {
    su_target_argv a{};
    __builtin_memcpy(&a, argv, sizeof(a));

    char buf[96];
    uint64_t i = 0;
    const char* prefix = "[su-target] uid=";
    for (; prefix[i] != '\0'; ++i) {
        buf[i] = prefix[i];
    }
    // uid는 0~수천 범위라 10진수 몇 자리면 충분하다 — 손으로 변환.
    char digits[10];
    uint64_t ndigits = 0;
    uint32_t v = a.uid;
    if (v == 0) {
        digits[ndigits++] = '0';
    }
    while (v > 0) {
        digits[ndigits++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    while (ndigits > 0) {
        buf[i++] = digits[--ndigits];
    }
    const char* suffix = " super=";
    for (uint64_t j = 0; suffix[j] != '\0'; ++j) {
        buf[i++] = suffix[j];
    }
    buf[i++] = a.is_super ? '1' : '0';
    const char* suffix2 = " guest=";
    for (uint64_t j = 0; suffix2[j] != '\0'; ++j) {
        buf[i++] = suffix2[j];
    }
    buf[i++] = a.is_guest ? '1' : '0';
    buf[i++] = '\n';
    debug_log(buf, i);

    if (a.is_guest || a.is_jail) {
        uint64_t identity = (a.is_guest ? 1ull : 0) | (a.is_jail ? 2ull : 0);

        uapi::message outside_req{};
        outside_req.label = k_op_open;
        pack_bytes(outside_req.regs, k_path_budget, "/etc/denied.txt",
                   cstr_len("/etc/denied.txt"));
        outside_req.regs[3] = identity;
        uapi::message outside_reply{};
        do_syscall(uapi::k_syscall_ipc_call, k_vfs_handle, reinterpret_cast<uint64_t>(&outside_req),
                   reinterpret_cast<uint64_t>(&outside_reply));
        bool outside_denied = (outside_reply.regs[1] == 5);  // GUEST_DENIED, fs-protocol.md v3 §3.
        const char* m1 = outside_denied ? "[su-target] guest open outside denied=1\n"
                                          : "[su-target] guest open outside denied=0\n";
        debug_log(m1, cstr_len(m1));

        uapi::message inside_req{};
        inside_req.label = k_op_open;
        pack_bytes(inside_req.regs, k_path_budget, "/home/guest1/allowed.txt",
                   cstr_len("/home/guest1/allowed.txt"));
        inside_req.regs[3] = identity;
        uapi::message inside_reply{};
        do_syscall(uapi::k_syscall_ipc_call, k_vfs_handle, reinterpret_cast<uint64_t>(&inside_req),
                   reinterpret_cast<uint64_t>(&inside_reply));
        bool inside_ok = (inside_reply.regs[1] == 0 && inside_reply.handle_count == 1);
        const char* m2 = inside_ok ? "[su-target] guest open inside ok=1\n"
                                     : "[su-target] guest open inside ok=0\n";
        debug_log(m2, cstr_len(m2));
    }

    quiet_exit();
}

// vfs→memfs로 파일을 열고, 그 응답으로 위임받은 memfs 핸들에 직접
// 쓰고 다시 읽어 내용이 일치하는지 확인한다(fs-protocol.md §2). 결과는
// sys_debug_log로만 관찰 가능하다(klog가 유저에 노출된 적이 없어서 —
// 이 파일 상단 주석 참고).
void run_vfs_roundtrip_test() {
    const char* path = "test.txt";
    const char* payload = "hello vfs";
    uint64_t payload_len = cstr_len(payload);

    uint64_t open_file_id = 0;
    uint32_t memfs_handle = 0;
    vfs_open(path, open_file_id, memfs_handle);
    if (memfs_handle == 0) {
        debug_log("[procsrv] vfs open failed\n", cstr_len("[procsrv] vfs open failed\n"));
        return;
    }

    // fs-protocol.md v3 §2.2(ADR-168) — OP_WRITE도 이제 pages[]다.
    for (uint64_t i = 0; i < k_page_size; ++i) {
        g_write_scratch[i] = (i < payload_len) ? static_cast<uint8_t>(payload[i]) : 0;
    }
    uapi::message write_req{};
    write_req.label = k_op_write;
    write_req.regs[0] = open_file_id;
    write_req.regs[1] = payload_len;
    write_req.page_count = 1;
    write_req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_write_scratch);
    write_req.pages[0].length = k_page_size;
    write_req.pages[0].mode = uapi::transfer_mode::copy;
    uapi::message write_reply{};
    do_syscall(uapi::k_syscall_ipc_call, memfs_handle, reinterpret_cast<uint64_t>(&write_req),
               reinterpret_cast<uint64_t>(&write_reply));
    bool write_ok = (write_reply.regs[1] == 0) && (write_reply.regs[0] == payload_len);

    // fs-protocol.md v2 §2.3(ADR-155 §2/ADR-159/ADR-161) — OP_READ의
    // 응답은 이제 pages[]로 온다. 커널이 sys_call이 돌아오기 전에
    // 이미 이 프로세스의 고정 슬롯에 매핑을 마쳐 뒀으므로,
    // read_reply.pages[0].vaddr을 그냥 읽으면 된다(별도 매핑/해제
    // 호출 불필요).
    uapi::message read_req{};
    read_req.label = k_op_read;
    read_req.regs[0] = open_file_id;
    read_req.regs[1] = payload_len;
    uapi::message read_reply{};
    do_syscall(uapi::k_syscall_ipc_call, memfs_handle, reinterpret_cast<uint64_t>(&read_req),
               reinterpret_cast<uint64_t>(&read_reply));
    bool read_ok = (read_reply.regs[1] == 0) && (read_reply.regs[0] == payload_len) &&
                    read_reply.page_count == 1 &&
                    bytes_equal(reinterpret_cast<const void*>(read_reply.pages[0].vaddr), payload,
                                payload_len);

    if (write_ok && read_ok) {
        const char* msg = "[procsrv] vfs write/read roundtrip ok=1\n";
        debug_log(msg, cstr_len(msg));
    } else {
        const char* msg = "[procsrv] vfs write/read roundtrip ok=0\n";
        debug_log(msg, cstr_len(msg));
    }
}

// M16(fs-protocol.md v2, ADR-057/129) — vfs의 마운트 테이블을 거쳐
// fat32/ext4 서버가 실제로 마운트한 이미지에서 파일을 열어 읽는다.
// tools/make-fs-test-images.sh가 각 이미지의 루트에 hello.txt를
// 미리 심어 두므로(내용은 fat32/ext4가 서로 다름), 그 내용이 그대로
// 읽히는지 확인한다 — memfs 경로와 달리 여기는 **호스트가 이미
// 써 둔 내용을 게스트가 처음 읽는** 시나리오다(FAT32/ext4 v1은
// 읽기전용이라 OP_WRITE가 없다).
void run_mounted_fs_read_test(const char* mount_path, const char* expected,
                               const char* log_prefix) {
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    vfs_open(mount_path, open_file_id, fs_handle);
    if (fs_handle == 0) {
        debug_log(log_prefix, cstr_len(log_prefix));
        const char* msg = " open failed\n";
        debug_log(msg, cstr_len(msg));
        return;
    }

    uapi::message read_req{};
    read_req.label = k_op_read;
    read_req.regs[0] = open_file_id;
    read_req.regs[1] = 4096;
    uapi::message read_reply{};
    do_syscall(uapi::k_syscall_ipc_call, fs_handle, reinterpret_cast<uint64_t>(&read_req),
               reinterpret_cast<uint64_t>(&read_reply));

    uint64_t expected_len = cstr_len(expected);
    bool read_ok = (read_reply.regs[1] == 0) && (read_reply.page_count == 1) &&
                    (read_reply.regs[0] == expected_len) &&
                    bytes_equal(reinterpret_cast<const void*>(read_reply.pages[0].vaddr), expected,
                                expected_len);

    debug_log(log_prefix, cstr_len(log_prefix));
    if (read_ok) {
        const char* msg = " read ok=1\n";
        debug_log(msg, cstr_len(msg));
    } else {
        const char* msg = " read ok=0\n";
        debug_log(msg, cstr_len(msg));
    }
}

// ---------- M19 — cfgsrv 클라이언트(registry.md, registry-decisions.md ADR-169) ----------
// handle 3 = initrun이 스폰 시점에 넣어 준 cfgsrv endpoint 프록시
// (lib/*.ini의 depends=vfs,cfgsrv — 나열 순서대로 handle 2, 3).
constexpr uint32_t k_cfgsrv_handle = 3;

constexpr uint32_t k_reg_op_open_table = 1;
constexpr uint32_t k_reg_op_create_table = 2;
constexpr uint32_t k_reg_op_delete_table = 3;
constexpr uint32_t k_reg_op_list_children = 4;
constexpr uint32_t k_reg_op_get_value = 5;
constexpr uint32_t k_reg_op_set_value = 6;
constexpr uint32_t k_reg_op_delete_value = 7;
constexpr uint32_t k_reg_op_list_values = 8;
constexpr uint32_t k_reg_op_set_permissions = 9;

constexpr uint64_t k_reg_err_ok = 0;
constexpr uint64_t k_reg_err_not_found = 1;
constexpr uint64_t k_reg_err_permission_denied = 2;

constexpr uint8_t k_reg_type_string = 0;

alignas(k_page_size) uint8_t g_cfg_path_buf[k_page_size] = {};
alignas(k_page_size) uint8_t g_cfg_key_buf[k_page_size] = {};
alignas(k_page_size) uint8_t g_cfg_value_buf[k_page_size] = {};

void fill_page_buf(uint8_t* buf, const char* text) {
    uint64_t len = cstr_len(text);
    for (uint64_t i = 0; i < k_page_size; ++i) {
        buf[i] = (i < len) ? static_cast<uint8_t>(text[i]) : 0;
    }
}

uint64_t reg_open_or_create(uint32_t op, uint32_t caller_uid, const char* caller_username,
                             const char* path, uint64_t& out_table_id) {
    fill_page_buf(g_cfg_path_buf, path);
    uapi::message req{};
    req.label = op;
    req.regs[0] = caller_uid;
    pack_bytes(&req.regs[1], sizeof(uint64_t), caller_username, cstr_len(caller_username));
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_path_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = uapi::transfer_mode::copy;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    out_table_id = reply.regs[1];
    return reply.regs[0];
}

uint64_t reg_get_string(uint32_t caller_uid, uint64_t table_id, const char* key, char* out_buf,
                         uint64_t out_cap, uint64_t& out_len) {
    fill_page_buf(g_cfg_key_buf, key);
    uapi::message req{};
    req.label = k_reg_op_get_value;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_key_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = uapi::transfer_mode::copy;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    if (reply.regs[0] != k_reg_err_ok) {
        return reply.regs[0];
    }
    out_len = reply.regs[2];
    if (out_len > out_cap) {
        out_len = out_cap;
    }
    const auto* src = reinterpret_cast<const uint8_t*>(reply.pages[0].vaddr);
    for (uint64_t i = 0; i < out_len; ++i) {
        out_buf[i] = static_cast<char>(src[i]);
    }
    return k_reg_err_ok;
}

uint64_t reg_set_string(uint32_t caller_uid, uint64_t table_id, const char* key,
                         const char* value) {
    fill_page_buf(g_cfg_key_buf, key);
    fill_page_buf(g_cfg_value_buf, value);
    uapi::message req{};
    req.label = k_reg_op_set_value;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.regs[2] = k_reg_type_string;
    req.regs[3] = cstr_len(value);
    req.page_count = 2;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_key_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = uapi::transfer_mode::copy;
    req.pages[1].vaddr = reinterpret_cast<uint64_t>(g_cfg_value_buf);
    req.pages[1].length = k_page_size;
    req.pages[1].mode = uapi::transfer_mode::copy;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    return reply.regs[0];
}

uint64_t reg_delete_value(uint32_t caller_uid, uint64_t table_id, const char* key) {
    fill_page_buf(g_cfg_key_buf, key);
    uapi::message req{};
    req.label = k_reg_op_delete_value;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_key_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = uapi::transfer_mode::copy;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    return reply.regs[0];
}

uint64_t reg_list_values(uint32_t caller_uid, uint64_t table_id, uint64_t& out_count) {
    uapi::message req{};
    req.label = k_reg_op_list_values;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    out_count = reply.regs[1];
    return reply.regs[0];
}

uint64_t reg_list_children(uint32_t caller_uid, const char* caller_username, const char* path,
                            uint64_t& out_count) {
    fill_page_buf(g_cfg_path_buf, path);
    uapi::message req{};
    req.label = k_reg_op_list_children;
    req.regs[0] = caller_uid;
    pack_bytes(&req.regs[1], sizeof(uint64_t), caller_username, cstr_len(caller_username));
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_path_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = uapi::transfer_mode::copy;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    out_count = reply.regs[1];
    return reply.regs[0];
}

uint64_t reg_delete_table(uint32_t caller_uid, const char* caller_username, const char* path) {
    fill_page_buf(g_cfg_path_buf, path);
    uapi::message req{};
    req.label = k_reg_op_delete_table;
    req.regs[0] = caller_uid;
    pack_bytes(&req.regs[1], sizeof(uint64_t), caller_username, cstr_len(caller_username));
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_path_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = uapi::transfer_mode::copy;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    return reply.regs[0];
}

uint64_t reg_set_permissions(uint32_t caller_uid, uint64_t table_id, uint32_t owner_uid,
                              uint8_t owner_rwx, uint8_t other_rwx) {
    uint8_t* p = g_cfg_path_buf;  // 재사용(경로 페이지 필요 없는 오퍼레이션).
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>((owner_uid >> (8 * i)) & 0xFF);
    }
    p[4] = 0;
    p[5] = 0;
    p[6] = 0;
    p[7] = 0;  // group_gid — ADR-169 §결정1, 항상 0.
    p[8] = owner_rwx;
    p[9] = 0;  // group_rwx — 항상 무시.
    p[10] = other_rwx;
    p[11] = 0;  // special_bits.
    for (uint64_t i = 12; i < k_page_size; ++i) {
        p[i] = 0;
    }
    uapi::message req{};
    req.label = k_reg_op_set_permissions;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_path_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = uapi::transfer_mode::copy;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    return reply.regs[0];
}

// M19 검증 목표(system-servers-bringup.md §M19) — "procsrv가 cfgsrv에
// 설정값을 쓰고 다시 읽는 왕복이 권한 모델대로 동작함을 확인". procsrv
// 자신은 root와 동등한 uid 0으로 cfgsrv를 부른다(registry.md §7의
// "procsrv가 이미 보유한 전체 권한 reg_table 핸들"과 같은 정신).
void run_cfgsrv_roundtrip_test() {
    const char* path = "@global/test/settings";
    uint64_t table_id = 0;
    uint64_t status = reg_open_or_create(k_reg_op_create_table, 0, "root", path, table_id);
    bool create_ok = (status == k_reg_err_ok) && (table_id != 0);

    bool set_ok = create_ok && (reg_set_string(0, table_id, "greeting", "hello cfgsrv") == k_reg_err_ok);

    char got[64];
    uint64_t got_len = 0;
    bool get_ok =
        set_ok &&
        (reg_get_string(0, table_id, "greeting", got, sizeof(got), got_len) == k_reg_err_ok) &&
        bytes_equal(got, "hello cfgsrv", cstr_len("hello cfgsrv")) && got_len == cstr_len("hello cfgsrv");

    const char* msg1 = (create_ok && set_ok && get_ok) ? "[procsrv] cfgsrv roundtrip ok=1\n"
                                                          : "[procsrv] cfgsrv roundtrip ok=0\n";
    debug_log(msg1, cstr_len(msg1));

    // 권한 모델 확인(1/2) — 소유자가 아닌 uid(test=1000)는 기본
    // 비공개(other_rwx=0) 테이블을 열 수 없어야 한다.
    uint64_t other_table_id = 0;
    uint64_t denied_status = reg_open_or_create(k_reg_op_open_table, 1000, "test", path, other_table_id);
    const char* msg2 = (denied_status == k_reg_err_permission_denied)
                           ? "[procsrv] cfgsrv permission denied before grant=1\n"
                           : "[procsrv] cfgsrv permission denied before grant=0\n";
    debug_log(msg2, cstr_len(msg2));

    // 권한 모델 확인(2/2) — 소유자(uid 0)가 set_permissions로 other에
    // 읽기 권한을 열어 주면, 그다음부터는 같은 비소유자가 읽을 수
    // 있어야 한다.
    bool chmod_ok = set_ok && (reg_set_permissions(0, table_id, 0, 0b111, 0b100) == k_reg_err_ok);
    uint64_t granted_table_id = 0;
    uint64_t granted_status =
        chmod_ok ? reg_open_or_create(k_reg_op_open_table, 1000, "test", path, granted_table_id)
                 : k_reg_err_permission_denied;
    char got2[64];
    uint64_t got2_len = 0;
    bool granted_get_ok =
        (granted_status == k_reg_err_ok) &&
        (reg_get_string(1000, granted_table_id, "greeting", got2, sizeof(got2), got2_len) ==
         k_reg_err_ok) &&
        bytes_equal(got2, "hello cfgsrv", cstr_len("hello cfgsrv"));
    const char* msg3 = granted_get_ok ? "[procsrv] cfgsrv permission granted after chmod=1\n"
                                        : "[procsrv] cfgsrv permission granted after chmod=0\n";
    debug_log(msg3, cstr_len(msg3));

    // registry.md §5의 나머지 오퍼레이션(list_values/list_children/
    // delete_value/delete_table)도 한 번씩 실제로 행사해 9종 전부가
    // 동작함을 확인한다.
    uint64_t value_count = 0;
    bool list_values_ok = set_ok && (reg_list_values(0, table_id, value_count) == k_reg_err_ok) &&
                           value_count == 1;

    uint64_t child_count = 0;
    bool list_children_ok =
        create_ok && (reg_list_children(0, "root", "@global/test", child_count) == k_reg_err_ok) &&
        child_count == 1;

    bool delete_value_ok = set_ok && (reg_delete_value(0, table_id, "greeting") == k_reg_err_ok);
    char got3[64];
    uint64_t got3_len = 0;
    bool delete_value_confirmed =
        delete_value_ok &&
        (reg_get_string(0, table_id, "greeting", got3, sizeof(got3), got3_len) == k_reg_err_not_found);

    bool delete_table_ok = create_ok && (reg_delete_table(0, "root", path) == k_reg_err_ok);
    uint64_t reopen_table_id = 0;
    bool delete_table_confirmed =
        delete_table_ok &&
        (reg_open_or_create(k_reg_op_open_table, 0, "root", path, reopen_table_id) ==
         k_reg_err_not_found);

    bool full_protocol_ok = list_values_ok && list_children_ok && delete_value_confirmed &&
                             delete_table_confirmed;
    const char* msg4 = full_protocol_ok ? "[procsrv] cfgsrv full protocol ok=1\n"
                                          : "[procsrv] cfgsrv full protocol ok=0\n";
    debug_log(msg4, cstr_len(msg4));
}

}  // namespace

extern "C" [[noreturn]] void _start(const void* argv_or_null) {
    // M18(security-model.md ADR-167) — su-target 역할 판별이 가장
    // 먼저다(이 파일 상단 주석의 argv 규약: magic 접두사가 있으면
    // su-target, argv==nullptr이면 self-exec 사본, 그 외는 정상
    // 부트 스폰).
    if (is_su_target_argv(argv_or_null)) {
        run_as_su_target(argv_or_null);
    }
    if (argv_or_null == nullptr) {
        quiet_exit();  // sys_fork+sys_exec으로 만들어진 사본.
    }

    const auto* self_info =
        reinterpret_cast<const uapi::m12_self_info*>(uapi::k_m12_self_info_user_vaddr);

    uint64_t fork_ret = do_syscall(uapi::k_syscall_fork, 0, 0, 0);
    if (fork_ret == 0) {
        // 자식 — sys_exec으로 자기 자신을 다시 실행(argv 없음 = 0,
        // 위 상단 주석의 규약대로 이 경로가 다음번 _start에서 사본으로
        // 식별된다).
        uapi::exec_request exec_req{};
        exec_req.elf_data = self_info->elf_addr;
        exec_req.elf_size = self_info->elf_size;
        do_syscall(uapi::k_syscall_exec, reinterpret_cast<uint64_t>(&exec_req), 0, 0);
        quiet_exit();  // exec 실패 시에만 도달.
    }

    // 부모 — fork/exec 왕복이 성공했다는 사실 자체가 M12의 검증
    // 목표다(커널의 process_ops.cpp가 이미 "[process] fork ok"/
    // "[process] exec ok"를 로그로 남긴다 — 어느 프로세스가 호출했는지
    // 구분하지 않는 일반 로그이므로 procsrv가 호출해도 그대로 관찰
    // 가능하다). M13의 검증 목표(VFS 경유 memfs 왕복)는 여기서 이어서
    // 확인한다.
    run_vfs_roundtrip_test();

    // M16 — tools/make-fs-test-images.sh가 심어 둔 내용과 정확히
    // 일치해야 한다(그 스크립트의 FAT32_CONTENT/EXT4_CONTENT).
    run_mounted_fs_read_test("/mnt/fat32/hello.txt", "hello fat32 world\n", "[procsrv] fat32");
    run_mounted_fs_read_test("/mnt/ext4/hello.txt", "hello ext4 world\n", "[procsrv] ext4");

    // M18(security-model.md ADR-167) — 경로→ELF 로더를 검증하고
    // (procsrv 자신의 ELF를 VFS에 쓰고 다시 읽어 재조립), guest
    // 격리도 실제 스폰된 프로세스로 확인한다. 로그인 서버가
    // OP_SU를 부르기 전에 로더가 준비돼 있어야 한다.
    run_loader_test(*self_info);
    run_guest_confinement_test();

    // M19(system-servers-bringup.md §M19, registry-decisions.md
    // ADR-169) — cfgsrv에 설정값을 쓰고 다시 읽는 왕복과 권한 모델
    // (owner/other RWX)이 실제로 동작하는지 확인한다. handle 3(cfgsrv)
    // 는 initrun이 이미 넣어 줬다(lib/*.ini의 depends=vfs,cfgsrv).
    run_cfgsrv_roundtrip_test();

    // M17(security-model.md ADR-165) — 여기서부터 procsrv가 처음으로
    // 진짜 서버가 된다. servers/login이 OP_LOGIN/OP_SU로 이 계정
    // 저장소와 위임 테이블에 묻는다.
    init_accounts();
    for (;;) {
        uapi::message in{};
        uint64_t recv_err = do_syscall(uapi::k_syscall_ipc_recv, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in), 0);
        uapi::message out{};
        if (recv_err == 0) {
            out.label = in.label;
            if (in.label == k_op_login) {
                handle_login(in, out);
            } else if (in.label == k_op_su) {
                handle_su(in, out);
            }
        }
        do_syscall(uapi::k_syscall_ipc_reply, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
