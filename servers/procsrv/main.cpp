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
#include <mc/procsrv_protocol.h>
#include <mc/syscall.h>

#include "musl_exec_target_blob.h"
#include "user_service_unit_blob.h"
#include "msh_blob.h"
#include "loop_test_blob.h"

namespace kernsrv::procsrv {

namespace {

// sys_process_spawn(create_endpoint=true)이 handle 1을 이 프로세스의
// endpoint로 채운다(ADR-152) — M17부터 servers/login이 OP_LOGIN으로
// 이 handle에 건다(security-model.md ADR-165). handle 2는 initrun이
// 스폰 시점에 넣어 준 vfs endpoint 프록시(lib/*.ini의 `depends=vfs`).
constexpr uint32_t k_own_endpoint_handle = 1;
constexpr uint32_t k_vfs_handle = 2;
// handle 3은 initrun이 넣어 준 cfgsrv endpoint 프록시(k_cfgsrv_handle,
// 아래 M19 섹션에서 선언). handle 4는 M20(ADR-171)엔 셸 endpoint
// 프록시였지만 M53(ADR-224)이 그 배선을 완전히 걷어냈다 — 지금은
// pipesrv endpoint 프록시다(lib/*.ini의 depends=vfs,cfgsrv,pipesrv,
// M54/ADR-225 — start_session_once()가 이 handle을 그대로 msh에게
// 넘겨줘야 해서 앞쪽에 선언한다).
constexpr uint32_t k_pipesrv_handle = 4;

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
    do_syscall(MC_SYSCALL_THREAD_EXIT, 0, 0, 0);
    for (;;) {
        asm volatile("pause");
    }
}

void debug_log(const char* msg, uint64_t len) {
    do_syscall(MC_SYSCALL_DEBUG_LOG, reinterpret_cast<uint64_t>(msg), len, 0);
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

// ---------- M27(real-libc-syscall-layer.md §M27, ADR-201) — 실제
// 프로세스 테이블 + 범용 wait/kill ----------
//
// procsrv.md §2가 정의한 process_entry의 이번 라운드 부분집합이다 —
// fd_table/quota_state/identity_badge 등 이 마일스톤이 다루지 않는
// 필드는 아직 없다(§10류 후속 확장 대상, 아래 done 보고의 "남긴
// 문제" 참고). pid는 procsrv가 발급하는 단순 증가 카운터이고,
// procsrv 자기 자신이 pid=1로 부트스트랩 등록된다.
enum class process_state : uint8_t {
    running = 0,
    zombie = 1,
};

constexpr uint32_t k_max_processes = 64;
constexpr uint32_t k_parent_none = 0;      // procsrv.md §2 — 0 = 부모 없음.
constexpr uint32_t k_procsrv_self_pid = 1;

struct process_entry {
    uint32_t pid = 0;              // 0 = 미사용 슬롯.
    uint32_t parent_pid = k_parent_none;
    uint32_t thread_handle = 0;    // procsrv 자신의 핸들 테이블 인덱스(sys_process_kill 대상). 0=없음(procsrv 자기 자신).
    process_state state = process_state::running;
    int32_t exit_code = 0;
};

process_entry g_processes[k_max_processes];
uint32_t g_next_pid = k_procsrv_self_pid;

uint32_t alloc_pid() {
    return g_next_pid++;
}

process_entry* find_process(uint32_t pid) {
    for (auto& p : g_processes) {
        if (p.pid == pid) {
            return &p;
        }
    }
    return nullptr;
}

process_entry* alloc_process_slot() {
    for (auto& p : g_processes) {
        if (p.pid == 0) {
            return &p;
        }
    }
    return nullptr;
}

// pid는 이미 alloc_pid()로 할당돼 있어야 한다(스폰 전에 argv에 미리
// 담아야 하는 경우가 있어 "번호 할당"과 "테이블 등록"을 분리했다 —
// 아래 run_general_process_table_test() 참고). 반환값은 실제로
// 슬롯에 들어갔는지(false=테이블 가득 참, k_max_processes=64) — M27의
// 기존 호출자들(procsrv 자신의 self-test)은 무시해도 안전하지만,
// M32의 handle_proc_fork_register()는 이 실패를 caller에게 그대로
// 보고해야 한다(MC_PROC_STATUS_TABLE_FULL).
bool insert_process_entry(uint32_t pid, uint32_t parent_pid, uint32_t thread_handle) {
    process_entry* slot = alloc_process_slot();
    if (slot == nullptr) {
        return false;
    }
    slot->pid = pid;
    slot->parent_pid = parent_pid;
    slot->thread_handle = thread_handle;
    slot->state = process_state::running;
    slot->exit_code = 0;
    return true;
}

// ADR-192 §결정3의 재부모화 메커니즘 — 대상(to_parent_pid)을
// 매개변수로 받는 일반 함수. M27은 실제 initrun 종료 트리거와
// 연결하지 않았다(아래 done 보고 참고) —
// run_reparenting_mechanism_test()가 합성 pid로 메커니즘 자체만
// 증명했다. M40(user-service-manager.md §M40)이 실제 트리거
// (handle_proc_adopt_orphans, svcmgr가 부른다)를 추가하며
// exclude_pid도 추가했다 — svcmgr 자신도 initrun의 고아라 이
// 호출 시점엔 parent_pid==from_parent_pid(k_parent_none)이지만,
// 자기 자신을 자기 부모로 만들면 안 된다(트리 루트는 부모가
// 없어야 정상). 반환값(재부모화된 개수)은 M40의 adopt_orphans
// 응답에 쓴다.
uint32_t reparent_children(uint32_t from_parent_pid, uint32_t to_parent_pid,
                            uint32_t exclude_pid = 0) {
    uint32_t count = 0;
    for (auto& p : g_processes) {
        if (p.pid != 0 && p.pid != exclude_pid && p.parent_pid == from_parent_pid) {
            p.parent_pid = to_parent_pid;
            ++count;
        }
    }
    return count;
}

// M40(user-service-manager.md §M40, ADR-192 §결정3/ADR-196 §결정5) —
// svcmgr가 자기 pid를 self_register로 확정한 뒤 부른다. M27이
// 잠정 처리해 둔 "initrun의 고아는 parent_pid=k_parent_none"을
// 실제 대상(svcmgr)으로 교체하는 진짜 트리거다. caller_pid는
// 다른 op들과 같은 self-asserted 모델(ADR-201)이다.
void handle_proc_adopt_orphans(const mc_message& in, mc_message& out) {
    uint32_t svcmgr_pid = static_cast<uint32_t>(in.regs[0]);
    uint32_t adopted = reparent_children(k_parent_none, svcmgr_pid, /*exclude_pid=*/svcmgr_pid);
    out.regs[0] = MC_PROC_STATUS_OK;
    out.regs[1] = adopted;
}

// OPEN-54 해소 — 정확한 regs[] 배치(mc/procsrv_protocol.h의 @wire-op
// 마크업과 정확히 일치해야 한다). caller_pid는 호출자가 스스로
// 주장하는 값이다(ADR-201 §결정2 — badge 기반 검증은 이 라운드
// 범위 밖).
void handle_proc_wait(const mc_message& in, mc_message& out) {
    uint32_t target_pid = static_cast<uint32_t>(in.regs[0]);
    uint32_t caller_pid = static_cast<uint32_t>(in.regs[1]);
    process_entry* target = find_process(target_pid);
    if (target == nullptr || target->parent_pid != caller_pid) {
        out.regs[0] = MC_PROC_STATUS_NOT_FOUND;
        return;
    }
    if (target->state != process_state::zombie) {
        out.regs[0] = MC_PROC_STATUS_STILL_RUNNING;
        return;
    }
    out.regs[0] = MC_PROC_STATUS_OK;
    out.regs[1] = static_cast<uint64_t>(static_cast<int64_t>(target->exit_code));
    target->pid = 0;  // POSIX wait() 관례 — 회수 후 좀비 슬롯을 반환한다.
}

// M27은 caller_pid의 kill 권한(예: 같은 uid/조상 관계) 검사를 생략한다
// — pid를 아는 누구나 kill을 요청할 수 있다. 실제 권한 모델은 이
// 계획의 범위 밖(procsrv.md §10류 후속 확장 대상)이라 명시적으로
// 남긴다.
void handle_proc_kill(const mc_message& in, mc_message& out) {
    uint32_t target_pid = static_cast<uint32_t>(in.regs[0]);
    process_entry* target = find_process(target_pid);
    if (target == nullptr) {
        out.regs[0] = MC_PROC_STATUS_NOT_FOUND;
        return;
    }
    do_syscall(MC_SYSCALL_PROCESS_KILL, target->thread_handle, 0, 0);
    target->state = process_state::zombie;
    target->exit_code = -9;  // SIGKILL 관례(신호 계층 자체는 M36).
    out.regs[0] = MC_PROC_STATUS_OK;
}

// M55(musl-userland-porting.md §M55, ADR-227) — 호출자(msh)가 자기
// 자식에게 이미 mc_signal_send()로 직접 시그널을 보낸 뒤 그 사실만
// 알려준다. handle_proc_kill과 달리 MC_SYSCALL_PROCESS_KILL을 다시
// 시도하지 않는다 — msh의 파이프라인 자식은 항상 thread_handle=0
// 으로 등록돼 있어(procsrv가 fork_register된 자식을 가리키는 진짜
// 커널 핸들을 원천적으로 못 갖는다, ADR-227 배경 참고) 그 syscall이
// 어차피 무효 핸들로 조용히 no-op된다. caller_pid는 다른 오퍼레이션
// (OP_WAIT 등)과 같은 자기주장 모델이다(ADR-201 §결정2).
void handle_proc_report_signaled(const mc_message& in, mc_message& out) {
    uint32_t target_pid = static_cast<uint32_t>(in.regs[0]);
    uint32_t caller_pid = static_cast<uint32_t>(in.regs[1]);
    uint32_t signal_number = static_cast<uint32_t>(in.regs[2]);
    process_entry* target = find_process(target_pid);
    if (target == nullptr || target->parent_pid != caller_pid) {
        out.regs[0] = MC_PROC_STATUS_NOT_FOUND;
        return;
    }
    target->state = process_state::zombie;
    target->exit_code = -static_cast<int32_t>(signal_number);
    out.regs[0] = MC_PROC_STATUS_OK;
}

void handle_proc_exit_report(const mc_message& in) {
    uint32_t pid = static_cast<uint32_t>(in.regs[0]);
    int32_t exit_code = static_cast<int32_t>(in.regs[1]);
    process_entry* p = find_process(pid);
    if (p != nullptr) {
        p->state = process_state::zombie;
        p->exit_code = exit_code;
    }
}

// M32(real-libc-syscall-layer.md §M32, libmc mc_getpid()) — 호출자가
// 아직 procsrv에 알려지지 않은 프로세스(procsrv 자신이 직접 스폰하지
// 않은 일반 프로세스 — 예: initrun이 스폰한 musl-hello)일 때, 최초로
// 자신의 pid를 물을 때 그 자리에서 등록한다. parent_pid=k_parent_none
// (procsrv.md §2 "0 = 부모 없음"과 같은 관례 — 이 경로로 등록되는
// 프로세스는 진짜 부모를 모른다, ADR-201과 같은 정신의 확장).
void handle_proc_self_register(mc_message& out) {
    uint32_t pid = alloc_pid();
    if (!insert_process_entry(pid, k_parent_none, /*thread_handle=*/0)) {
        out.regs[0] = MC_PROC_STATUS_TABLE_FULL;
        return;
    }
    out.regs[0] = MC_PROC_STATUS_OK;
    out.regs[1] = pid;
}

// M32 — libmc mc_fork()가 실제 sys_fork(MC_SYSCALL_FORK) **이전에**
// 호출한다. 자식의 pid를 미리 확정해 두면, sys_fork가 COW로 복제하는
// 호출자의 스택(그 pid를 담은 지역변수)을 통해 부모/자식 양쪽이
// 별도 통신 없이 같은 값을 보게 된다(libs/mc/src/ipc/procsrv_client.c
// mc_fork() 주석 참고). thread_handle=0(모름)으로 등록한다 — 이
// 자식은 procsrv가 직접 스폰한 게 아니라 진짜 sys_fork가 만들어서,
// M22의 sys_process_kill 대상 핸들을 procsrv가 얻을 길이 없다(OPEN-67
// 과 같은 정신 — kill()은 이 경로로 만들어진 자식에는 이 라운드에서
// 지원하지 않는다, wait()/exit_report만 지원).
void handle_proc_fork_register(const mc_message& in, mc_message& out) {
    uint32_t caller_pid = static_cast<uint32_t>(in.regs[0]);
    uint32_t child_pid = alloc_pid();
    if (!insert_process_entry(child_pid, caller_pid, /*thread_handle=*/0)) {
        out.regs[0] = MC_PROC_STATUS_TABLE_FULL;
        return;
    }
    out.regs[0] = MC_PROC_STATUS_OK;
    out.regs[1] = child_pid;
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

// M53(musl-userland-porting.md §M53, ADR-224) — 로그인 성공 시
// 이제 minicore 네이티브 셸(ADR-170, 부팅 시 이미 떠서 자기 handle
// 1에서 sys_ipc_recv로 블록 중이던 것을 OP_START로 깨우는 방식)이
// 아니라, 포팅된 실제 musl 셸(userland/msh)을 그 자리에서 진짜로
// fork+exec 스폰한다 — M39가 원래 세운 목표(포팅된 바이너리가
// 로그인 후 셸을 맡는다)를 이번에 완주한다. msh는 더 이상 부팅
// 시점 서비스가 아니다(servers/CMakeLists.txt에서 --service=msh=
// 제거) — 이 스폰이 유일한 진입점이다. 이번 라운드도 세션 하나만
// 다루므로 두 번째 호출을 막는 기존 정적 플래그 가드는 그대로
// 둔다(msh는 자기테스트를 마치면 종료하므로 두 번째 로그인이 같은
// msh 인스턴스를 다시 깨울 방법이 없다 — 매번 새로 스폰해야 하는데,
// 이번 라운드는 "세션 하나" 검증만 목표라 그 확장은 다루지 않는다).
bool g_session_started = false;

void start_session_once() {
    if (g_session_started) {
        return;
    }
    g_session_started = true;
    // msh는 실제 musl 프로그램이라 execve()/fork()/waitpid()/pipe()로
    // procsrv/pipesrv에게 되돌아와야 한다 — syscall_shim.c의 고정
    // 핸들 관례(MC_VFS_HANDLE=2, MC_PROCSRV_HANDLE=3,
    // MC_PIPESRV_HANDLE=4)와 맞추려면 inherited_handles[0]=vfs,
    // [1]=procsrv 자신, [2]=pipesrv(순서 그대로) — create_endpoint=
    // true가 먼저 handle 1을 차지하므로 [0]이 2, [1]이 3, [2]가 4가
    // 된다(userland/msh가 예전엔 initrun의 --depends=msh:vfs,procsrv
    // 로만 받던 것에, M54(ADR-225)의 pipe() 지원을 위해 pipesrv를
    // 더한 것 — 이제는 procsrv 자신이 스폰자가 됐을 뿐이다). M54
    // 실행 중 발견: pipesrv를 빼먹으면 mc_pipe_create()가 존재하지
    // 않는 handle 4에 IPC를 걸어, 그 syscall이 조용히 실패하고 남은
    // 회신 버퍼(0으로 미리 채워 둔 것)를 그대로 읽어 버린다 —
    // MC_PIPE_STATUS_OK가 마침 0이라 "성공했는데 파이프 id가 둘 다
    // 0"이라는 혼란스러운 증상으로 나타났다(0은 pipesrv 자신이
    // "항상 무효"로 예약해 둔 값인데도).
    mc_process_spawn_request req{};
    req.elf_data = reinterpret_cast<uint64_t>(g_msh_elf);
    req.elf_size = g_msh_elf_len;
    req.linux_abi_stack = 1;  // musl crt_arch.h가 요구하는 초기 스택(M28/ADR-183).
    req.create_endpoint = true;
    req.inherited_handle_count = 3;
    req.inherited_handles[0].src_handle = k_vfs_handle;
    req.inherited_handles[0].rights_mask = MC_RIGHT_CAN_SEND;
    req.inherited_handles[1].src_handle = k_own_endpoint_handle;
    req.inherited_handles[1].rights_mask = MC_RIGHT_CAN_SEND;
    req.inherited_handles[2].src_handle = k_pipesrv_handle;
    req.inherited_handles[2].rights_mask = MC_RIGHT_CAN_SEND;
    do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&req), 0, 0);
    const char* msg = "[procsrv] shell session start ok=1\n";
    debug_log(msg, cstr_len(msg));
}

// M43(user-service-manager.md §M43, docs/design/security-model.md
// ADR-218 §결정6) — 로그인 성공마다 채워지는 작은 원형 큐. svcmgr가
// 백그라운드 스레드로 폴링한다(op_poll_login_event) — procsrv는
// 단일 요청-응답 루프라(OPEN-67) 진짜 블로킹을 못 해 비블로킹
// 폴링 모델을 그대로 재사용한다.
constexpr uint32_t k_max_login_events = 8;
struct login_event {
    uint32_t uid;
    char username[8];
};
login_event g_login_events[k_max_login_events];
uint32_t g_login_event_head = 0;
uint32_t g_login_event_count = 0;

void push_login_event(const account& acc) {
    if (g_login_event_count >= k_max_login_events) {
        return;  // 큐 가득 참 — 드묾(YAGNI), 이 이벤트는 버린다.
    }
    uint32_t idx = (g_login_event_head + g_login_event_count) % k_max_login_events;
    g_login_events[idx].uid = acc.uid;
    for (uint64_t i = 0; i < sizeof(acc.username); ++i) {
        g_login_events[idx].username[i] = acc.username[i];
    }
    ++g_login_event_count;
}

bool pop_login_event(login_event& out) {
    if (g_login_event_count == 0) {
        return false;
    }
    out = g_login_events[g_login_event_head];
    g_login_event_head = (g_login_event_head + 1) % k_max_login_events;
    --g_login_event_count;
    return true;
}

void handle_login(const mc_message& in, mc_message& out) {
    for (const account& acc : g_accounts) {
        if (bytes_equal(&in.regs[0], acc.username, sizeof(acc.username)) &&
            bytes_equal(&in.regs[1], acc.password, sizeof(acc.password))) {
            out.regs[0] = k_login_status_ok;
            start_session_once();
            push_login_event(acc);
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
// M32(real-libc-syscall-layer.md §M32) — procsrv 자신이 musl-exec-target
// ELF를 컴파일 시점 데이터로 심게 되면서(tools/bin2c.py) procsrv 자신의
// ELF도 131072를 넘어서(139072바이트, 2026-09-10) servers/fs/memfs::
// k_max_file_bytes와 함께 262144로 늘렸다 — 이 값은 BSS 배열(아래
// g_reassembled) 크기라 늘려도 procsrv 자신의 ELF **파일** 크기에는
// 영향이 없다(BSS는 파일에 저장되지 않는다). M52 — echo/ls/cat 블롭
// 셋을 더 심으며 procsrv 자신의 ELF가 418640바이트까지 커져 262144를
// 다시 넘었다(memfs::k_max_file_bytes만 늘리고 이 값을 깜빡해 loader
// roundtrip이 "ok=0"으로 계속 실패했다 — read_elf_from_vfs가 이
// 버퍼 크기(max_len)에서 그대로 잘려 write_ok=true인데도
// g_reassembled_size != self_info.elf_size가 됐다) — 함께 1048576로
// 올린다.
constexpr uint64_t k_max_reassembled_bytes = 1048576;  // servers/fs/memfs::k_max_file_bytes와 일치.
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

// M22(general-purpose-completion.md §M22, ADR-178) — wait/kill 검증용
// 데모 두 개도 su-target과 같은 방식(procsrv 자신의 재조립 ELF를
// 다른 argv 마커로 다시 스폰)으로 만든다 — 별도 CMake 타깃을 새로
// 만들지 않는다.
constexpr uint32_t k_wait_target_magic = 0x57414954;  // "WAIT"
struct wait_target_argv {
    uint32_t magic = 0;
    int32_t exit_code = 0;
};

bool is_wait_target_argv(const void* argv) {
    if (argv == nullptr) {
        return false;
    }
    uint32_t magic;
    __builtin_memcpy(&magic, argv, sizeof(magic));
    return magic == k_wait_target_magic;
}

constexpr uint32_t k_kill_target_magic = 0x4B494C4C;  // "KILL"
struct kill_target_argv {
    uint32_t magic = 0;
};

bool is_kill_target_argv(const void* argv) {
    if (argv == nullptr) {
        return false;
    }
    uint32_t magic;
    __builtin_memcpy(&magic, argv, sizeof(magic));
    return magic == k_kill_target_magic;
}

// M27(real-libc-syscall-layer.md §M27, ADR-201) — 위 M22 자기테스트와
// 달리, procsrv 자신의 endpoint가 아니라 **범용 proc_op::wait/kill**
// (mc/procsrv_protocol.h)을 실제로 왕복시키는 두 역할. role=0(A,
// 부모)은 wait_target_pid/kill_target_pid로 procsrv에게 물어보고,
// role=1(B, 자식)은 exit_report로 자신의 종료를 procsrv에게 스스로
// 보고한다 — procsrv가 두 pid를 미리 알고(스폰 순서: kill 대상→B→A)
// argv에 담아 준다(이 파일 상단 주석과 같은 관례, "procsrv가 A/B
// 양쪽을 다 스폰하므로 서로의 pid를 사전에 알 수 있다"는 이번
// 라운드의 단순화 — 아래 run_general_process_table_test() 참고).
constexpr uint32_t k_m27_test_magic = 0x4D323757;  // "M27W"
constexpr int32_t k_m27_expected_exit_code = 77;
struct m27_test_argv {
    uint32_t magic = 0;
    uint32_t self_pid = 0;
    uint32_t wait_target_pid = 0;  // role=0(A)만 사용.
    uint32_t kill_target_pid = 0;  // role=0(A)만 사용.
    int32_t exit_code = 0;         // role=1(B)만 사용.
    uint8_t role = 0;              // 0=A(부모), 1=B(자식).
};

bool is_m27_test_argv(const void* argv) {
    if (argv == nullptr) {
        return false;
    }
    uint32_t magic;
    __builtin_memcpy(&magic, argv, sizeof(magic));
    return magic == k_m27_test_magic;
}

// inherited_handles[0]이 항상 procsrv 자신의 수신 endpoint(handle 1,
// k_own_endpoint_handle)의 프록시다 — create_endpoint=true가 먼저
// handle 1을 차지하므로 상속된 첫 핸들은 handle 2가 된다(ADR-152의
// 고정 순서, spawn_su_target과 같은 관례).
constexpr uint32_t k_m27_procsrv_call_handle = 2;

[[noreturn]] void run_as_m27_child_b(const m27_test_argv& a) {
    mc_message req{};
    req.label = MC_PROC_OP_EXIT_REPORT;
    req.regs[0] = a.self_pid;
    req.regs[1] = static_cast<uint64_t>(static_cast<int64_t>(a.exit_code));
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_m27_procsrv_call_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    quiet_exit();
}

[[noreturn]] void run_as_m27_parent_a(const m27_test_argv& a) {
    bool wait_ok = false;
    int32_t got_exit_code = 0;
    // procsrv의 수신 루프는 한 번에 한 메시지씩만 처리하므로(단일
    // 스레드), B의 exit_report가 이 wait보다 늦게 도착할 수 있다 —
    // MC_PROC_STATUS_STILL_RUNNING을 받으면 재시도한다(비블로킹
    // 폴링, ADR-201 §결정3).
    for (uint32_t attempt = 0; attempt < 64 && !wait_ok; ++attempt) {
        mc_message req{};
        req.label = MC_PROC_OP_WAIT;
        req.regs[0] = a.wait_target_pid;
        req.regs[1] = a.self_pid;
        mc_message reply{};
        do_syscall(MC_SYSCALL_IPC_CALL, k_m27_procsrv_call_handle,
                   reinterpret_cast<uint64_t>(&req), reinterpret_cast<uint64_t>(&reply));
        if (reply.regs[0] == MC_PROC_STATUS_OK) {
            wait_ok = true;
            got_exit_code = static_cast<int32_t>(reply.regs[1]);
        }
    }
    bool ok = wait_ok && got_exit_code == k_m27_expected_exit_code;
    const char* m1 =
        ok ? "[procsrv] m27 wait exit_code ok=1\n" : "[procsrv] m27 wait exit_code ok=0\n";
    debug_log(m1, cstr_len(m1));

    mc_message kreq{};
    kreq.label = MC_PROC_OP_KILL;
    kreq.regs[0] = a.kill_target_pid;
    kreq.regs[1] = a.self_pid;
    mc_message kreply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_m27_procsrv_call_handle, reinterpret_cast<uint64_t>(&kreq),
               reinterpret_cast<uint64_t>(&kreply));
    bool kill_ok = (kreply.regs[0] == MC_PROC_STATUS_OK);
    const char* m2 = kill_ok ? "[procsrv] m27 kill ok=1\n" : "[procsrv] m27 kill ok=0\n";
    debug_log(m2, cstr_len(m2));

    quiet_exit();
}

[[noreturn]] void run_as_m27_test(const void* argv) {
    m27_test_argv a{};
    __builtin_memcpy(&a, argv, sizeof(a));
    if (a.role == 1) {
        run_as_m27_child_b(a);
    }
    run_as_m27_parent_a(a);
}

// M23(general-purpose-completion.md §M23, ADR-179) — fork+exec 뒤에도
// 상속받은 fd(open_file_id+memfs_handle)로 이어 읽을 수 있는지
// 검증하는 타깃. open_file_id/memfs_handle을 argv로 그대로 넘긴다 —
// exec가 handle_table을 건드리지 않으므로(process_ops.cpp::exec_current
// 참고) memfs_handle 값 자체는 fork 시점 그대로 유효해야 한다(이
// argv 전달은 "그 값이 여전히 유효한가"를 확인하는 수단일 뿐,
// 핸들을 새로 만들어 주는 것이 아니다).
constexpr uint32_t k_fd_continue_magic = 0x46444331;  // "FDC1"
struct fd_continue_argv {
    uint32_t magic = 0;
    uint64_t open_file_id = 0;
    uint32_t memfs_handle = 0;
};

bool is_fd_continue_argv(const void* argv) {
    if (argv == nullptr) {
        return false;
    }
    uint32_t magic;
    __builtin_memcpy(&magic, argv, sizeof(magic));
    return magic == k_fd_continue_magic;
}

// vfs에 path를 열어 open_file_id/fs_handle을 얻는다(identity=0 —
// 로더 자신은 guest/jail이 아니다). 실패하면 fs_handle=0.
void vfs_open(const char* path, uint64_t& out_open_file_id, uint32_t& out_fs_handle) {
    mc_message req{};
    req.label = k_op_open;
    pack_bytes(req.regs, k_path_budget, path, cstr_len(path));
    req.regs[3] = 0;
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_vfs_handle, reinterpret_cast<uint64_t>(&req),
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
        mc_message req{};
        req.label = k_op_write;
        req.regs[0] = open_file_id;
        req.regs[1] = chunk;
        req.page_count = 1;
        req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_write_scratch);
        req.pages[0].length = k_page_size;
        req.pages[0].mode = MC_TRANSFER_COPY;
        mc_message reply{};
        do_syscall(MC_SYSCALL_IPC_CALL, fs_handle, reinterpret_cast<uint64_t>(&req),
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
        mc_message req{};
        req.label = k_op_read;
        req.regs[0] = open_file_id;
        req.regs[1] = k_page_size;
        mc_message reply{};
        do_syscall(MC_SYSCALL_IPC_CALL, fs_handle, reinterpret_cast<uint64_t>(&req),
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
void run_loader_test(const mc_m12_self_info& self_info) {
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

    mc_process_spawn_request req{};
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
    req.inherited_handles[0].rights_mask = MC_RIGHT_CAN_SEND;
    do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&req), 0, 0);
}

void handle_su(const mc_message& in, mc_message& out) {
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

// M23(general-purpose-completion.md §M23, ADR-179) — fd_continue_argv로
// exec된 procsrv 사본의 역할. argv로 넘겨받은 memfs_handle/open_file_id
// 로 OP_READ를 계속한다 — fork() 직전에 부모가 이미 앞부분("hello")을
// 읽어 서버 쪽 read_cursor를 옮겨 뒀으므로, 이 자식이 요청하는 나머지
// 4바이트(" vfs")가 정확히 오면(그리고 fork가 handle_table을 복제해
// 이 handle 번호가 실제로 같은 memfs endpoint를 가리키면) fd 상속이
// 실제로 동작한다는 뜻이다.
[[noreturn]] void run_as_fd_continue_target(const void* argv) {
    fd_continue_argv a{};
    __builtin_memcpy(&a, argv, sizeof(a));

    mc_message read_req{};
    read_req.label = k_op_read;
    read_req.regs[0] = a.open_file_id;
    read_req.regs[1] = 4;
    mc_message read_reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, a.memfs_handle, reinterpret_cast<uint64_t>(&read_req),
               reinterpret_cast<uint64_t>(&read_reply));
    bool ok = (read_reply.regs[1] == 0) && (read_reply.regs[0] == 4) &&
              read_reply.page_count == 1 &&
              bytes_equal(reinterpret_cast<const void*>(read_reply.pages[0].vaddr), " vfs", 4);
    const char* m = ok ? "[procsrv] fd inherited continue read ok=1\n"
                         : "[procsrv] fd inherited continue read ok=0\n";
    debug_log(m, cstr_len(m));
    quiet_exit();
}

// M23 — 위 타깃을 실제로 fork+exec해 fd 상속을 검증한다.
// run_vfs_roundtrip_test()가 이미 만들어 둔 "test.txt"("hello vfs",
// 9바이트)를 **새로운 open_file_id로 다시 연다**(run_vfs_roundtrip_test
// 자신의 open_file_id는 이미 그 전체를 다 읽어 커서가 파일 끝에
// 있다 — open_file_id별로 독립된 커서이므로 서로 간섭하지 않는다,
// servers/fs/memfs/main.cpp의 g_opens[] 참고). g_loader_ok가
// 필요하므로 반드시 run_loader_test() 이후에 불러야 한다.
void run_fd_inheritance_test() {
    if (!g_loader_ok) {
        return;
    }

    uint64_t open_file_id = 0;
    uint32_t memfs_handle = 0;
    vfs_open("test.txt", open_file_id, memfs_handle);
    if (memfs_handle == 0) {
        const char* m = "[procsrv] fd inheritance open failed\n";
        debug_log(m, cstr_len(m));
        return;
    }

    // 앞부분 5바이트("hello")만 읽어 서버 쪽 read_cursor를 5로
    // 옮겨 둔다 — 나머지(" vfs")를 자식이 이어 읽는 것이 검증
    // 대상이다.
    mc_message read_req{};
    read_req.label = k_op_read;
    read_req.regs[0] = open_file_id;
    read_req.regs[1] = 5;
    mc_message read_reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, memfs_handle, reinterpret_cast<uint64_t>(&read_req),
               reinterpret_cast<uint64_t>(&read_reply));
    bool first_ok = (read_reply.regs[1] == 0) && (read_reply.regs[0] == 5) &&
                     read_reply.page_count == 1 &&
                     bytes_equal(reinterpret_cast<const void*>(read_reply.pages[0].vaddr), "hello",
                                 5);
    if (!first_ok) {
        const char* m = "[procsrv] fd inheritance first read failed\n";
        debug_log(m, cstr_len(m));
        return;
    }

    uint64_t fork_ret = do_syscall(MC_SYSCALL_FORK, 0, 0, 0);
    if (fork_ret == 0) {
        // 자식 — M23(ADR-179)의 fork() 수정으로 이 시점에 이미
        // memfs_handle이 그대로 유효하다. exec로 완전히 새 이미지로
        // 뛰어들어(handle_table은 exec가 건드리지 않는다) "그래도 그
        // 값이 유효한가"까지 함께 확인한다.
        fd_continue_argv fargv{};
        fargv.magic = k_fd_continue_magic;
        fargv.open_file_id = open_file_id;
        fargv.memfs_handle = memfs_handle;

        mc_exec_request exec_req{};
        exec_req.elf_data = reinterpret_cast<uint64_t>(g_reassembled);
        exec_req.elf_size = g_reassembled_size;
        exec_req.argv_blob = reinterpret_cast<uint64_t>(&fargv);
        exec_req.argv_size = sizeof(fargv);
        do_syscall(MC_SYSCALL_EXEC, reinterpret_cast<uint64_t>(&exec_req), 0, 0);
        quiet_exit();  // exec 실패 시에만 도달.
    }
    // 부모는 그대로 다음 자기테스트로 진행한다 — fork 성공 자체는
    // 커널이 이미 "[process] fork ok"로 로그를 남긴다.
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

        mc_message outside_req{};
        outside_req.label = k_op_open;
        pack_bytes(outside_req.regs, k_path_budget, "/etc/denied.txt",
                   cstr_len("/etc/denied.txt"));
        outside_req.regs[3] = identity;
        mc_message outside_reply{};
        do_syscall(MC_SYSCALL_IPC_CALL, k_vfs_handle, reinterpret_cast<uint64_t>(&outside_req),
                   reinterpret_cast<uint64_t>(&outside_reply));
        bool outside_denied = (outside_reply.regs[1] == 5);  // GUEST_DENIED, fs-protocol.md v3 §3.
        const char* m1 = outside_denied ? "[su-target] guest open outside denied=1\n"
                                          : "[su-target] guest open outside denied=0\n";
        debug_log(m1, cstr_len(m1));

        mc_message inside_req{};
        inside_req.label = k_op_open;
        pack_bytes(inside_req.regs, k_path_budget, "/home/guest1/allowed.txt",
                   cstr_len("/home/guest1/allowed.txt"));
        inside_req.regs[3] = identity;
        mc_message inside_reply{};
        do_syscall(MC_SYSCALL_IPC_CALL, k_vfs_handle, reinterpret_cast<uint64_t>(&inside_req),
                   reinterpret_cast<uint64_t>(&inside_reply));
        bool inside_ok = (inside_reply.regs[1] == 0 && inside_reply.handle_count == 1);
        const char* m2 = inside_ok ? "[su-target] guest open inside ok=1\n"
                                     : "[su-target] guest open inside ok=0\n";
        debug_log(m2, cstr_len(m2));
    }

    quiet_exit();
}

// M22(general-purpose-completion.md §M22, ADR-178) — wait() 검증용
// 자식. **최초 설계는 procsrv 자신의 메인 endpoint(handle 1)에
// Call로 exit_code를 "먼저 알리는" 것이었으나, 실제 QEMU 실행에서
// servers/login의 OP_LOGIN Call이 procsrv가 이 자기테스트에 도달하기
// **전에** 이미 그 endpoint의 waiting_callers에 큐잉돼 있어서
// (login은 procsrv의 진행 상태와 무관하게 즉시 Call을 건다), procsrv
// 가 sys_recv를 부르는 순간 login의 메시지를 이 wait 자기테스트가
// 가로채 버리는 것을 실제로 재현했다(그 결과 login 쪽은 텅 빈
// 응답을 "성공(0)"으로 오인하고, 이 자기테스트는 login의 페이로드를
// exit_code로 오인해 실패로 보였다). **그래서 방향을 뒤집었다**:
// 자식이 procsrv의 공유 endpoint로 먼저 말 거는 대신, procsrv가
// create_endpoint=true로 이 자식만을 위한 **새** endpoint를 만들어
// (handle 1 = 이 자식 전용, 아무도 공유하지 않음) 자식에게 "네
// 상태를 알려달라"고 먼저 Call하고 이 자식이 Reply로 exit_code를
// 담아 응답한다 — 충돌할 다른 큐가 원천적으로 없다.
constexpr uint32_t k_wait_target_own_endpoint_handle = 1;

[[noreturn]] void run_as_wait_target(const void* argv) {
    wait_target_argv a{};
    __builtin_memcpy(&a, argv, sizeof(a));

    mc_message ask{};
    do_syscall(MC_SYSCALL_IPC_RECV, k_wait_target_own_endpoint_handle,
               reinterpret_cast<uint64_t>(&ask), 0);
    mc_message reply{};
    reply.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(a.exit_code));
    do_syscall(MC_SYSCALL_IPC_REPLY, reinterpret_cast<uint64_t>(&reply), 0, 0);
    quiet_exit();
}

// M22 — kill() 검증용 자식. 아무 handle도 물려받지 않는다(procsrv가
// 이 스레드를 직접 sys_process_kill로 끝내지, IPC로 대화할 일이
// 없다). 스스로는 절대 끝나지 않는 게 정상이지만(강제 종료로만
// 없어져야 검증이 성립한다), M21에서 겪은 "무한 루프가 나머지
// 부팅을 굶긴다" 실수를 반복하지 않도록 충분히 큰 유한 반복 뒤
// 스스로도 끝나게 방어적으로 막아 둔다(kill이 실패해도 시스템
// 전체가 멈추지는 않는다).
[[noreturn]] void run_as_kill_target(const void*) {
    const unsigned long k_iterations = 200000000ul;
    for (unsigned long i = 0; i < k_iterations; ++i) {
        asm volatile("nop");
    }
    quiet_exit();
}

// M22 — 위 둘을 실제로 스폰해 wait/kill 왕복을 검증한다. g_loader_ok/
// g_reassembled(run_loader_test가 이미 채워 둔 procsrv 자신의 재조립
// ELF 바이트)를 spawn_su_target과 같은 방식으로 재사용한다 — 반드시
// run_loader_test() 이후에 불러야 한다.
void run_process_lifecycle_test() {
    if (!g_loader_ok) {
        return;
    }

    // --- wait: 자식이 보낸 exit_code를 정확히 회수하는지 확인 ---
    wait_target_argv wargv{};
    wargv.magic = k_wait_target_magic;
    wargv.exit_code = 42;

    mc_process_spawn_request wreq{};
    wreq.elf_data = reinterpret_cast<uint64_t>(g_reassembled);
    wreq.elf_size = g_reassembled_size;
    wreq.argv_blob = reinterpret_cast<uint64_t>(&wargv);
    wreq.argv_size = sizeof(wargv);
    wreq.create_endpoint = true;  // 이 자식만의 새 endpoint — 위 run_as_wait_target 주석 참고.
    do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&wreq), 0, 0);

    mc_message ask{};
    mc_message notify{};
    do_syscall(MC_SYSCALL_IPC_CALL, wreq.out_endpoint_proxy_handle,
               reinterpret_cast<uint64_t>(&ask), reinterpret_cast<uint64_t>(&notify));

    bool wait_ok = (static_cast<int64_t>(notify.regs[0]) == wargv.exit_code);
    const char* m1 =
        wait_ok ? "[procsrv] wait exit_code ok=1\n" : "[procsrv] wait exit_code ok=0\n";
    debug_log(m1, cstr_len(m1));

    // --- kill: 대상 스레드 핸들을 얻어 강제 종료를 요청한다 ---
    kill_target_argv kargv{};
    kargv.magic = k_kill_target_magic;

    mc_process_spawn_request kreq{};
    kreq.elf_data = reinterpret_cast<uint64_t>(g_reassembled);
    kreq.elf_size = g_reassembled_size;
    kreq.argv_blob = reinterpret_cast<uint64_t>(&kargv);
    kreq.argv_size = sizeof(kargv);
    do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&kreq), 0, 0);

    uint64_t kill_err =
        do_syscall(MC_SYSCALL_PROCESS_KILL, kreq.out_thread_handle, 0, 0);
    const char* m2 =
        (kill_err == 0) ? "[procsrv] kill requested ok=1\n" : "[procsrv] kill requested ok=0\n";
    debug_log(m2, cstr_len(m2));
}

// M27(real-libc-syscall-layer.md §M27, ADR-201) — 위 run_process_lifecycle_test()
// 와 달리, procsrv 자신의 process_entry 테이블+범용 proc_op::wait/kill
// (mc/procsrv_protocol.h)을 실제로 왕복시킨다. 세 프로세스를 순서대로
// 스폰한다: C(kill 대상, 기존 kill_target_argv 재사용)→B(exit_report로
// 스스로 종료를 보고)→A(B/C의 pid를 이미 알고 wait/kill을 실제로
// 호출). procsrv가 A/B/C 전부를 스폰하므로 서로의 pid를 스폰 시점에
// 미리 알려줄 수 있다(이번 라운드의 단순화 — done 보고 참고).
void run_general_process_table_test() {
    if (!g_loader_ok) {
        return;
    }

    // procsrv 자기 자신을 pid=1로 부트스트랩 등록한다(아직 안 됐다면).
    if (find_process(k_procsrv_self_pid) == nullptr) {
        insert_process_entry(k_procsrv_self_pid, k_parent_none, /*thread_handle=*/0);
    }

    // A의 pid를 먼저 할당한다(아직 스폰하지 않았어도 번호만 필요) —
    // B/C를 "A의 자식"으로 등록해야 A의 OP_WAIT(caller_pid=자기 자신)
    // 권한 검사(handle_proc_wait의 parent_pid 비교)가 통과한다. A를
    // procsrv 자신의 자식으로 등록하는 것과는 별개다(A는 procsrv가
    // 실제로 스폰했으므로 parent_pid=k_procsrv_self_pid가 맞다 —
    // "누가 물리적으로 스폰했는가"와 "테이블상 부모가 누구인가"가
    // 다른 것은 procsrv 자신이 A/B/C 모두를 대신 스폰해 주는 이번
    // 라운드의 단순화 때문이다, done 보고 참고).
    uint32_t a_pid = alloc_pid();

    // --- C: kill 대상(기존 kill_target_argv, 오래 도는 busy loop) ---
    kill_target_argv cargv{};
    cargv.magic = k_kill_target_magic;
    mc_process_spawn_request creq{};
    creq.elf_data = reinterpret_cast<uint64_t>(g_reassembled);
    creq.elf_size = g_reassembled_size;
    creq.argv_blob = reinterpret_cast<uint64_t>(&cargv);
    creq.argv_size = sizeof(cargv);
    do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&creq), 0, 0);
    uint32_t c_pid = alloc_pid();
    insert_process_entry(c_pid, a_pid, creq.out_thread_handle);

    // --- B: exit_report로 스스로 종료를 procsrv에 알린다 ---
    uint32_t b_pid = alloc_pid();
    m27_test_argv bargv{};
    bargv.magic = k_m27_test_magic;
    bargv.role = 1;
    bargv.self_pid = b_pid;
    bargv.exit_code = k_m27_expected_exit_code;
    mc_process_spawn_request breq{};
    breq.elf_data = reinterpret_cast<uint64_t>(g_reassembled);
    breq.elf_size = g_reassembled_size;
    breq.argv_blob = reinterpret_cast<uint64_t>(&bargv);
    breq.argv_size = sizeof(bargv);
    breq.create_endpoint = true;  // handle 1 = B 자신의 새 endpoint(이번 시나리오는 안 쓰지만 관례 유지).
    breq.inherited_handle_count = 1;
    breq.inherited_handles[0].src_handle = k_own_endpoint_handle;  // procsrv 자신의 수신 endpoint(handle 1) — B/A가 handle 2로 받는다.
    breq.inherited_handles[0].rights_mask = MC_RIGHT_CAN_SEND;
    do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&breq), 0, 0);
    insert_process_entry(b_pid, a_pid, breq.out_thread_handle);

    // --- A: B/C의 pid를 이미 알고 wait/kill을 실제로 호출한다 ---
    m27_test_argv aargv{};
    aargv.magic = k_m27_test_magic;
    aargv.role = 0;
    aargv.self_pid = a_pid;
    aargv.wait_target_pid = b_pid;
    aargv.kill_target_pid = c_pid;
    mc_process_spawn_request areq{};
    areq.elf_data = reinterpret_cast<uint64_t>(g_reassembled);
    areq.elf_size = g_reassembled_size;
    areq.argv_blob = reinterpret_cast<uint64_t>(&aargv);
    areq.argv_size = sizeof(aargv);
    areq.create_endpoint = true;
    areq.inherited_handle_count = 1;
    areq.inherited_handles[0].src_handle = k_own_endpoint_handle;
    areq.inherited_handles[0].rights_mask = MC_RIGHT_CAN_SEND;
    do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&areq), 0, 0);
    insert_process_entry(a_pid, k_procsrv_self_pid, areq.out_thread_handle);
}

// ADR-192 §결정3의 재부모화 메커니즘(대상은 매개변수) 자체를
// 증명한다 — 실제 initrun 종료 트리거와의 연결은 이 마일스톤 범위
// 밖이라(done 보고 참고) 합성 pid로만 호출한다.
void run_reparenting_mechanism_test() {
    constexpr uint32_t k_synthetic_old_parent = 999;
    uint32_t child1 = alloc_pid();
    insert_process_entry(child1, k_synthetic_old_parent, /*thread_handle=*/0);
    uint32_t child2 = alloc_pid();
    insert_process_entry(child2, k_synthetic_old_parent, /*thread_handle=*/0);

    reparent_children(k_synthetic_old_parent, /*to_parent_pid=*/k_parent_none);

    process_entry* p1 = find_process(child1);
    process_entry* p2 = find_process(child2);
    bool ok = p1 != nullptr && p2 != nullptr && p1->parent_pid == k_parent_none &&
              p2->parent_pid == k_parent_none;
    const char* m =
        ok ? "[procsrv] reparent mechanism ok=1\n" : "[procsrv] reparent mechanism ok=0\n";
    debug_log(m, cstr_len(m));

    // 실제 프로세스가 아닌 테스트 전용 슬롯이므로 회수한다.
    if (p1 != nullptr) {
        p1->pid = 0;
    }
    if (p2 != nullptr) {
        p2->pid = 0;
    }
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
    mc_message write_req{};
    write_req.label = k_op_write;
    write_req.regs[0] = open_file_id;
    write_req.regs[1] = payload_len;
    write_req.page_count = 1;
    write_req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_write_scratch);
    write_req.pages[0].length = k_page_size;
    write_req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message write_reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, memfs_handle, reinterpret_cast<uint64_t>(&write_req),
               reinterpret_cast<uint64_t>(&write_reply));
    bool write_ok = (write_reply.regs[1] == 0) && (write_reply.regs[0] == payload_len);

    // fs-protocol.md v2 §2.3(ADR-155 §2/ADR-159/ADR-161) — OP_READ의
    // 응답은 이제 pages[]로 온다. 커널이 sys_call이 돌아오기 전에
    // 이미 이 프로세스의 고정 슬롯에 매핑을 마쳐 뒀으므로,
    // read_reply.pages[0].vaddr을 그냥 읽으면 된다(별도 매핑/해제
    // 호출 불필요).
    mc_message read_req{};
    read_req.label = k_op_read;
    read_req.regs[0] = open_file_id;
    read_req.regs[1] = payload_len;
    mc_message read_reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, memfs_handle, reinterpret_cast<uint64_t>(&read_req),
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

// M32(real-libc-syscall-layer.md §M32) — musl-hello가 fork()+execve()
// 로 실행할 "다른 이미지"의 원본 바이트(userland/musl-exec-target,
// procsrv 자신의 컴파일 시점 데이터로 tools/bin2c.py가 심어 둠 —
// servers/procsrv/CMakeLists.txt 참고)를 VFS(memfs)에 미리 써 둔다.
// write_elf_to_vfs는 이미 M18(su-target)이 만들어 둔 범용 헬퍼다 —
// 새로 만들 필요가 없었다. musl-hello는 vfs 하나에만 의존해도(같은
// initrun 스폰 순서 관례, M31의 "test.txt"와 같은 이유) 이 시점에는
// 이미 이 파일이 준비돼 있다.
void run_exec_target_seed() {
    bool ok = write_elf_to_vfs("musl-exec-target.elf", g_musl_exec_target_elf,
                               g_musl_exec_target_elf_len);
    const char* msg =
        ok ? "[procsrv] musl-exec-target seed ok=1\n" : "[procsrv] musl-exec-target seed ok=0\n";
    debug_log(msg, cstr_len(msg));
}

// M55(musl-userland-porting.md §M55, ADR-227) — 위 musl-exec-target과
// 같은 이유로 VFS에 미리 써 둔다 — msh가 job control 자기테스트로
// execve("/bin/loop-test", ...)할 수 있으려면 부팅 시점에 이미
// VFS에 있어야 한다(M56/ADR-228부터 coreutils는 빌트인이 돼 이
// 자리에 남는 유일한 대상이다).
void run_loop_test_seed() {
    bool ok = write_elf_to_vfs("/bin/loop-test", g_loop_test_elf, g_loop_test_elf_len);
    const char* msg = ok ? "[procsrv] loop-test seed ok=1\n" : "[procsrv] loop-test seed ok=0\n";
    debug_log(msg, cstr_len(msg));
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

    mc_message read_req{};
    read_req.label = k_op_read;
    read_req.regs[0] = open_file_id;
    read_req.regs[1] = 4096;
    mc_message read_reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, fs_handle, reinterpret_cast<uint64_t>(&read_req),
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
// handle 3 = initrun이 스폰 시점에 넣어 준 cfgsrv endpoint 프록시 —
// 나열 순서(lib/*.ini의 depends=vfs,cfgsrv,pipesrv) 그대로 handle
// 2/3/4다(handle 4=k_pipesrv_handle은 이 파일 위쪽에서 이미
// 선언했다 — start_session_once()가 일찍 써야 해서).
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
    mc_message req{};
    req.label = op;
    req.regs[0] = caller_uid;
    pack_bytes(&req.regs[1], sizeof(uint64_t), caller_username, cstr_len(caller_username));
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_path_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    out_table_id = reply.regs[1];
    return reply.regs[0];
}

uint64_t reg_get_string(uint32_t caller_uid, uint64_t table_id, const char* key, char* out_buf,
                         uint64_t out_cap, uint64_t& out_len) {
    fill_page_buf(g_cfg_key_buf, key);
    mc_message req{};
    req.label = k_reg_op_get_value;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_key_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
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
    mc_message req{};
    req.label = k_reg_op_set_value;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.regs[2] = k_reg_type_string;
    req.regs[3] = cstr_len(value);
    req.page_count = 2;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_key_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = MC_TRANSFER_COPY;
    req.pages[1].vaddr = reinterpret_cast<uint64_t>(g_cfg_value_buf);
    req.pages[1].length = k_page_size;
    req.pages[1].mode = MC_TRANSFER_COPY;
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    return reply.regs[0];
}

uint64_t reg_delete_value(uint32_t caller_uid, uint64_t table_id, const char* key) {
    fill_page_buf(g_cfg_key_buf, key);
    mc_message req{};
    req.label = k_reg_op_delete_value;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_key_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    return reply.regs[0];
}

uint64_t reg_list_values(uint32_t caller_uid, uint64_t table_id, uint64_t& out_count) {
    mc_message req{};
    req.label = k_reg_op_list_values;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    out_count = reply.regs[1];
    return reply.regs[0];
}

uint64_t reg_list_children(uint32_t caller_uid, const char* caller_username, const char* path,
                            uint64_t& out_count) {
    fill_page_buf(g_cfg_path_buf, path);
    mc_message req{};
    req.label = k_reg_op_list_children;
    req.regs[0] = caller_uid;
    pack_bytes(&req.regs[1], sizeof(uint64_t), caller_username, cstr_len(caller_username));
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_path_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    out_count = reply.regs[1];
    return reply.regs[0];
}

uint64_t reg_delete_table(uint32_t caller_uid, const char* caller_username, const char* path) {
    fill_page_buf(g_cfg_path_buf, path);
    mc_message req{};
    req.label = k_reg_op_delete_table;
    req.regs[0] = caller_uid;
    pack_bytes(&req.regs[1], sizeof(uint64_t), caller_username, cstr_len(caller_username));
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_path_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
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
    mc_message req{};
    req.label = k_reg_op_set_permissions;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_cfg_path_buf);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_cfgsrv_handle, reinterpret_cast<uint64_t>(&req),
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

// M43(user-service-manager.md §M43, docs/design/security-model.md
// ADR-218) — su/sudo(ADR-093)의 @global/system/delegates와는 분리된,
// "내 몫의 유저 서비스만" 좁게 위임하는 별도 테이블
// @global/system/service-delegates/<계정명>. 키는 항상 "svcmgr"
// 하나뿐이고, 값은 {u64 granted_at, u8 mode}뿐이다(mode=0=permanent
// 만 v1 지원, ADR-218 §3). 자가서비스 grant/revoke는 그 계정 자신이
// cfgsrv set_value/delete_value를 직접 불러 하므로(§결정2) procsrv
// 에는 조회만 있다.
// 실행 중 발견(2026-09-10) — 처음엔 "@global/system/service-delegates/
// <계정명>"으로 뒀지만, cfgsrv::normalize_path/schema_matches가
// "@global/..." 경로의 스키마를 항상 "global" 문자열 자체로 고정
// 취급해(계정명과 무관), CREATE_TABLE이 caller_uid!=0인 계정에게는
// **절대** 통과하지 않는다(uid=0/root만 @global/* 아래에 테이블을
// 만들 수 있다 — 의도된 동작이다, cfgsrv의 기존 "@계정/..." 개인
// 스키마와 겹치지 않게 하는 설계). 계정 자신이 grant를 자가서비스로
// 할 수 있어야 하므로(ADR-218 §결정2), 그 계정 **자신의** 스키마
// (@<계정명>/...)로 옮겼다 — schema_matches가 caller_username과
// 정확히 일치하는 스키마만 비-root CREATE_TABLE을 허용한다.
bool check_service_delegation(const account& target) {
    if (target.is_guest || target.is_jail) {
        return false;  // ADR-218 §5 — guest/jail은 이 위임도 무효.
    }
    char path[64] = {};
    path[0] = '@';
    uint64_t name_len = cstr_len(target.username);
    for (uint64_t i = 0; i < name_len && i < sizeof(path) - 1; ++i) {
        path[1 + i] = target.username[i];
    }
    uint64_t p = 1 + name_len;
    const char* suffix = "/system/service-delegate";
    for (uint64_t i = 0; suffix[i] != '\0' && p + i < sizeof(path) - 1; ++i) {
        path[p + i] = suffix[i];
    }
    p += cstr_len(suffix);
    path[p] = '\0';

    uint64_t table_id = 0;
    if (reg_open_or_create(k_reg_op_open_table, 0, "root", path, table_id) != k_reg_err_ok) {
        return false;  // 위임 테이블 자체가 없음 — 위임 없음.
    }
    uint8_t entry[16] = {};
    uint64_t got_len = 0;
    if (reg_get_string(0, table_id, "svcmgr", reinterpret_cast<char*>(entry), sizeof(entry),
                        got_len) != k_reg_err_ok ||
        got_len < 9) {
        return false;  // 위임 항목 없음.
    }
    uint8_t mode = entry[8];  // {u64 granted_at, u8 mode} 레이아웃.
    return mode == 0;  // permanent(0)만 v1.
}

// ADR-218 §결정6 — svcmgr 전용, 비블로킹 폴링.
void handle_proc_poll_login_event(mc_message& out) {
    login_event ev{};
    if (!pop_login_event(ev)) {
        out.regs[0] = MC_PROC_STATUS_NOT_FOUND;
        return;
    }
    out.regs[0] = MC_PROC_STATUS_OK;
    out.regs[1] = ev.uid;
    uint64_t packed = 0;
    for (uint64_t i = 0; i < sizeof(ev.username); ++i) {
        packed |= static_cast<uint64_t>(static_cast<uint8_t>(ev.username[i])) << (8 * i);
    }
    out.regs[2] = packed;
}

// ADR-217/218 — caller_badge는 메인 루프가 sys_recv의 badge 출력
// 인자로 실제로 받아 넘겨준 값이다(자기주장이 아니다). 이 값이
// MC_PROCSRV_SERVICE_DELEGATION_BADGE와 정확히 일치해야만(=이
// 핸들이 initrun이 svcmgr 스폰 시점에 스탬핑해 준 바로 그 것) 나머지
// 확인(계정 존재+위임)으로 넘어간다 — 그 외 caller_badge는 즉시 거부.
void handle_proc_spawn_delegated_unit(const mc_message& in, mc_message& out, uint64_t caller_badge) {
    if (caller_badge != MC_PROCSRV_SERVICE_DELEGATION_BADGE) {
        out.regs[0] = MC_PROC_STATUS_PERMISSION_DENIED;
        return;
    }
    char username[9] = {};
    uint64_t packed = in.regs[0];
    for (uint64_t i = 0; i < 8; ++i) {
        username[i] = static_cast<char>((packed >> (8 * i)) & 0xFF);
    }
    const account* target = find_account_by_username(username);
    if (target == nullptr || !check_service_delegation(*target)) {
        out.regs[0] = MC_PROC_STATUS_PERMISSION_DENIED;
        return;
    }

    // svcmgr가 regs로 넘긴 elf_data 포인터는 svcmgr **자신의**
    // 주소공간을 가리켜 여기서 그대로 역참조할 수 없다(서로 다른
    // 프로세스) — procsrv 자신이 같은 ELF를 컴파일 시점 데이터로
    // 심어 둔 것을 대신 쓴다(servers/procsrv/CMakeLists.txt).
    // ADR-193의 준비완료 핸드셰이크는 연결하지 않는다(OPEN-73,
    // mc/procsrv_protocol.h 주석 참고) — create_endpoint=false.
    mc_process_spawn_request req{};
    req.elf_data = reinterpret_cast<uint64_t>(g_user_service_unit_elf);
    req.elf_size = g_user_service_unit_elf_len;
    uint64_t err = do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&req), 0, 0);
    if (err != 0) {
        out.regs[0] = MC_PROC_STATUS_NOT_FOUND;
        return;
    }
    out.regs[0] = MC_PROC_STATUS_OK;
    out.regs[1] = req.out_thread_handle;
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
    // M22(general-purpose-completion.md §M22) — su-target과 같은 자리,
    // 같은 이유로 가장 먼저 판별한다.
    if (is_wait_target_argv(argv_or_null)) {
        run_as_wait_target(argv_or_null);
    }
    if (is_kill_target_argv(argv_or_null)) {
        run_as_kill_target(argv_or_null);
    }
    // M23(general-purpose-completion.md §M23) — 위와 같은 자리, 같은
    // 이유.
    if (is_fd_continue_argv(argv_or_null)) {
        run_as_fd_continue_target(argv_or_null);
    }
    // M27(real-libc-syscall-layer.md §M27) — 위와 같은 자리, 같은 이유.
    if (is_m27_test_argv(argv_or_null)) {
        run_as_m27_test(argv_or_null);
    }
    if (argv_or_null == nullptr) {
        quiet_exit();  // sys_fork+sys_exec으로 만들어진 사본.
    }

    // M40(user-service-manager.md §M40, ADR-192 §결정3) — procsrv
    // 자기 자신을 pid=1로 무조건 등록한다(예전엔
    // run_general_process_table_test() 안에서 자기테스트에 성공했을
    // 때만 등록됐다 — 그마저도 procsrv 자신의 재실행 사본에서만
    // 의미 있었지, 진짜 부팅 경로에는 아무 커널 서버도 procsrv에
    // self_register하지 않아 svcmgr의 adopt_orphans가 재부모화할
    // 실제 대상이 하나도 없었다는 것을 실행 중 발견했다 — VFS/devmgr/
    // cfgsrv 등은 지금도 procsrv를 전혀 모른다). procsrv 자신을
    // Unix의 pid 1과 같은 자리로 등록해 두면, 최소한 이 하나는 항상
    // 관찰 가능한 재부모화 대상이 된다.
    insert_process_entry(k_procsrv_self_pid, k_parent_none, /*thread_handle=*/0);

    const auto* self_info =
        reinterpret_cast<const mc_m12_self_info*>(MC_M12_SELF_INFO_USER_VADDR);

    uint64_t fork_ret = do_syscall(MC_SYSCALL_FORK, 0, 0, 0);
    if (fork_ret == 0) {
        // 자식 — sys_exec으로 자기 자신을 다시 실행(argv 없음 = 0,
        // 위 상단 주석의 규약대로 이 경로가 다음번 _start에서 사본으로
        // 식별된다).
        mc_exec_request exec_req{};
        exec_req.elf_data = self_info->elf_addr;
        exec_req.elf_size = self_info->elf_size;
        do_syscall(MC_SYSCALL_EXEC, reinterpret_cast<uint64_t>(&exec_req), 0, 0);
        quiet_exit();  // exec 실패 시에만 도달.
    }

    // 부모 — fork/exec 왕복이 성공했다는 사실 자체가 M12의 검증
    // 목표다(커널의 process_ops.cpp가 이미 "[process] fork ok"/
    // "[process] exec ok"를 로그로 남긴다 — 어느 프로세스가 호출했는지
    // 구분하지 않는 일반 로그이므로 procsrv가 호출해도 그대로 관찰
    // 가능하다). M13의 검증 목표(VFS 경유 memfs 왕복)는 여기서 이어서
    // 확인한다.
    run_vfs_roundtrip_test();
    run_exec_target_seed();
    run_loop_test_seed();

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

    // M23(general-purpose-completion.md §M23, ADR-179) — fork()가
    // handle_table을 실제로 복제하는지, exec() 이후에도 그 fd가
    // 계속 유효한지 검증한다. run_vfs_roundtrip_test()가 만들어 둔
    // test.txt가 필요하다(이미 위에서 실행됨).
    run_fd_inheritance_test();

    // M19(system-servers-bringup.md §M19, registry-decisions.md
    // ADR-169) — cfgsrv에 설정값을 쓰고 다시 읽는 왕복과 권한 모델
    // (owner/other RWX)이 실제로 동작하는지 확인한다. handle 3(cfgsrv)
    // 는 initrun이 이미 넣어 줬다(lib/*.ini의 depends=vfs,cfgsrv).
    run_cfgsrv_roundtrip_test();

    // M22(general-purpose-completion.md §M22, ADR-178) — 프로세스
    // 생명주기(자식의 exit_code 회수 + 강제 종료)를 검증한다.
    run_process_lifecycle_test();

    // M27(real-libc-syscall-layer.md §M27, ADR-201) — 실제
    // process_entry 테이블 위의 범용 proc_op::wait/kill과 재부모화
    // 메커니즘을 검증한다. run_general_process_table_test()가 스폰한
    // A/B/C는 아래 서비스 루프(k_own_endpoint_handle)에 자신들의
    // wait/kill/exit_report 메시지를 보내므로, 이 루프가 실제로
    // 시작돼야 그 메시지들이 처리된다 — 이 두 함수는 스폰/등록만
    // 하고 돌아온다.
    run_general_process_table_test();
    run_reparenting_mechanism_test();

    // M17(security-model.md ADR-165) — 여기서부터 procsrv가 처음으로
    // 진짜 서버가 된다. servers/login이 OP_LOGIN/OP_SU로 이 계정
    // 저장소와 위임 테이블에 묻는다.
    init_accounts();
    for (;;) {
        mc_message in{};
        uint64_t caller_badge = 0;
        // M43(ADR-217) — a3에 out-포인터를 넘겨 badge를 실제로 받는다
        // (지금까지는 항상 0을 넘겨 버려 왔다, kernel/arch/x86_64/
        // syscall.cpp 참고).
        uint64_t recv_err = do_syscall(MC_SYSCALL_IPC_RECV, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in),
                                        reinterpret_cast<uint64_t>(&caller_badge));
        mc_message out{};
        if (recv_err == 0) {
            out.label = in.label;
            if (in.label == k_op_login) {
                handle_login(in, out);
            } else if (in.label == k_op_su) {
                handle_su(in, out);
            } else if (in.label == MC_PROC_OP_WAIT) {
                handle_proc_wait(in, out);
            } else if (in.label == MC_PROC_OP_KILL) {
                handle_proc_kill(in, out);
            } else if (in.label == MC_PROC_OP_EXIT_REPORT) {
                handle_proc_exit_report(in);
            } else if (in.label == MC_PROC_OP_SELF_REGISTER) {
                handle_proc_self_register(out);
            } else if (in.label == MC_PROC_OP_FORK_REGISTER) {
                handle_proc_fork_register(in, out);
            } else if (in.label == MC_PROC_OP_POLL_LOGIN_EVENT) {
                handle_proc_poll_login_event(out);
            } else if (in.label == MC_PROC_OP_SPAWN_DELEGATED_UNIT) {
                handle_proc_spawn_delegated_unit(in, out, caller_badge);
            } else if (in.label == MC_PROC_OP_ADOPT_ORPHANS) {
                handle_proc_adopt_orphans(in, out);
            } else if (in.label == MC_PROC_OP_REPORT_SIGNALED) {
                handle_proc_report_signaled(in, out);
            }
        }
        do_syscall(MC_SYSCALL_IPC_REPLY, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}

}  // namespace kernsrv::procsrv
