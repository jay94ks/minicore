// servers/svcmgr/main.cpp — 유저 서비스 관리자 데몬(user-service-manager.md
// §M40~M42, docs/design/boot-and-drivers.md ADR-192 §결정2/3, ADR-196,
// registry-decisions.md ADR-197). initrun이 "커널 서버" 전부를
// 기동한 뒤 spawn하는 프로세스다(servers/CMakeLists.txt
// --depends=svcmgr:procsrv,cfgsrv) — initrun은 그 뒤 스스로
// 사라진다(ADR-131 §결정7).
//
// ADR-196 §결정1이 명시한 대로 libk+libmc만 링크한 순수 minicore
// 네이티브 서버다(musl 불필요) — procsrv/cfgsrv를 호출하는 또 하나의
// 유저 프로세스일 뿐, 새 커널/IPC 프리미티브를 하나도 추가하지
// 않는다.
//
// M40: 재부모화(ADR-192 §결정3, procsrv 새 wire op adopt_orphans).
// M41: `@global/system/services`(ADR-197)를 실제로 읽어 depends_on
// 위상정렬로 부팅 시 유닛을 spawn(exec_path는 아직 안 읽는다 — 등록된
// 유닛이 몇 개든 전부 같은 임베딩된 데모 ELF를 실행, M41 done 참고).
// M42(이 라운드): `mc/svcmgr_protocol.h`의 컨트롤 프로토콜
// (list/status/start/stop/restart/register/unregister)을 own
// endpoint 위에서 실제로 처리한다. op_stop/restart는 기존
// sys_process_kill(ADR-178)을 그대로 쓰고, op_register/unregister는
// M41이 만든 cfgsrv 클라이언트(set_value/delete_value)를 그대로
// 쓴다 — 새 종료/저장 메커니즘을 만들지 않는다(ADR-196 §결정7
// 근거 그대로). svcmgr는 별도로 권한을 검사하지 않는다(cfgsrv
// 자신의 ADR-062 권한 검사에 그대로 의존, ADR-197 §결정3).
#include <k/spinlock.hpp>

#include <mc/cfgsrv_client.h>
#include <mc/lifecycle_client.h>
#include <mc/procsrv_client.h>
#include <mc/procsrv_protocol.h>
#include <mc/svcmgr_protocol.h>
#include <mc/syscall.h>
#include <mc/util.h>

#include "svcmgr_demo_unit_blob.h"

namespace {

// servers/CMakeLists.txt의 --depends=svcmgr:procsrv,cfgsrv 순서 그대로
// handle 2/3(servers/login/main.cpp의 k_procsrv_handle=4와 같은 관례).
constexpr uint32_t k_procsrv_handle = 2;
constexpr uint32_t k_cfgsrv_handle = 3;

constexpr char k_services_table_path[] = "@global/system/services";
constexpr uint32_t k_max_units = 8;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

void debug_log(const char* msg) {
    do_syscall(MC_SYSCALL_DEBUG_LOG, reinterpret_cast<uint64_t>(msg), mc_cstr_len(msg), 0);
}

// name(NUL 안 보장 — 최대 len)을 그대로 로그에 잇는다. mc_svcmgr_service_unit::name
// 처럼 고정폭 배열에 담긴 이름을 사람이 읽는 로그에 그대로 낼 때 쓴다.
void debug_log_n(const char* data, uint64_t len) {
    do_syscall(MC_SYSCALL_DEBUG_LOG, reinterpret_cast<uint64_t>(data), len, 0);
}

bool cstr_equals(const char* a, const char* b) {
    uint64_t i = 0;
    for (;; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
        if (a[i] == '\0') {
            return true;
        }
    }
}

// haystack(길이 haystack_len, NUL로 구분된 이름들이 이어진 blob) 안에
// needle(NUL 종료 문자열)과 정확히 일치하는 세그먼트가 있는지 찾는다
// — list_values의 응답을 그대로 훑는다.
bool blob_contains_name(const char* haystack, uint64_t haystack_len, const char* needle) {
    uint64_t seg_start = 0;
    for (uint64_t i = 0; i <= haystack_len; ++i) {
        if (i == haystack_len || haystack[i] == '\0') {
            uint64_t seg_len = i - seg_start;
            uint64_t needle_len = mc_cstr_len(needle);
            if (seg_len == needle_len && mc_bytes_equal(haystack + seg_start, needle, seg_len)) {
                return true;
            }
            seg_start = i + 1;
        }
    }
    return false;
}

// M42(user-service-manager.md §M42) — svcmgr가 지금 알고 있는 유닛
// 각각의 런타임 상태(procsrv의 process_entry와 비슷한 자리지만,
// 이건 svcmgr 자신만 보는 로컬 상태다 — pid가 아니라 kernel thread
// 핸들로 직접 sys_process_kill을 걸 수 있어야 하므로, ADR-178/M22
// 패턴을 그대로 재사용한다). 부팅 시 load_units()가 읽은 유닛들과
// op_register로 나중에 추가된 유닛들이 여기 다 들어간다.
// M43(user-service-manager.md §M43, ADR-219) — account=""(빈 문자열)이면
// system-scope 인스턴스(M40~M42와 완전히 같다). account!=""이면
// per_account 유닛의 그 계정 몫 인스턴스다 — 같은 유닛 이름이 계정마다
// 독립된 슬롯을 갖는다(systemd의 unit@instance와 같은 정신).
struct runtime_unit {
    bool used = false;
    char name[32] = {};
    char account[32] = {};
    bool running = false;
    uint32_t thread_handle = 0;  // k_right_can_signal(=k_right_can_kill) 보유, running일 때만 유효.
};

runtime_unit g_runtime[k_max_units];

// M43 — g_runtime은 이제 메인 스레드(컨트롤 프로토콜 dispatch)와
// 로그인 감시 스레드(아래 login_watcher_entry) 둘 다가 건드린다 —
// M42까지는 단일 스레드라 락이 필요 없었다.
spinlock g_runtime_lock;

runtime_unit* find_runtime_locked(const char* name, const char* account) {
    for (auto& u : g_runtime) {
        if (u.used && cstr_equals(u.name, name) && cstr_equals(u.account, account)) {
            return &u;
        }
    }
    return nullptr;
}

runtime_unit* alloc_runtime_locked(const char* name, const char* account) {
    runtime_unit* existing = find_runtime_locked(name, account);
    if (existing != nullptr) {
        return existing;
    }
    for (auto& u : g_runtime) {
        if (!u.used) {
            u.used = true;
            mc_zero_bytes(u.name, sizeof(u.name));
            mc_pack_bytes(u.name, sizeof(u.name), name, mc_cstr_len(name));
            mc_zero_bytes(u.account, sizeof(u.account));
            mc_pack_bytes(u.account, sizeof(u.account), account, mc_cstr_len(account));
            u.running = false;
            u.thread_handle = 0;
            return &u;
        }
    }
    return nullptr;
}

// "<유닛명>@<계정명>" 또는 "<유닛명>"(system-scope, account="")을
// 분리한다(ADR-219 §결정4) — out_name/out_account 둘 다 32바이트.
void split_name_account(const char* combined, char* out_name, char* out_account) {
    mc_zero_bytes(out_name, 32);
    mc_zero_bytes(out_account, 32);
    uint64_t at = 0;
    bool found_at = false;
    for (uint64_t i = 0; combined[i] != '\0' && i < 63; ++i) {
        if (combined[i] == '@') {
            at = i;
            found_at = true;
            break;
        }
    }
    if (!found_at) {
        mc_pack_bytes(out_name, 32, combined, mc_cstr_len(combined));
        return;
    }
    uint64_t name_len = at < 31 ? at : 31;
    for (uint64_t i = 0; i < name_len; ++i) {
        out_name[i] = combined[i];
    }
    uint64_t acct_len = mc_cstr_len(combined + at + 1);
    uint64_t copy_len = acct_len < 31 ? acct_len : 31;
    for (uint64_t i = 0; i < copy_len; ++i) {
        out_account[i] = combined[at + 1 + i];
    }
}

// 데모 유닛 하나를 spawn하고 준비완료까지 기다린다(파일 상단 주석
// — exec_path는 아직 안 읽는다, 항상 같은 임베딩된 ELF). 성공하면
// *out_thread_handle에 sys_process_kill(ADR-178) 대상 핸들을 채운다
// (M40의 out_thread_handle 자리 — process_spawn이 이미 이 관례를
// 쓴다).
bool spawn_unit_and_wait_ready(uint32_t& out_thread_handle) {
    mc_process_spawn_request req{};
    req.elf_data = reinterpret_cast<uint64_t>(g_svcmgr_demo_unit_elf);
    req.elf_size = g_svcmgr_demo_unit_elf_len;
    req.create_endpoint = true;
    uint64_t err = do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&req), 0, 0);
    if (err != 0) {
        return false;
    }
    mc_wait_ready(req.out_endpoint_proxy_handle);
    out_thread_handle = req.out_thread_handle;
    return true;
}

// M41 — @global/system/services 전체를 읽어 units[]에 채운다(최대
// k_max_units개, YAGNI). 반환값은 실제로 읽은 유닛 수.
uint32_t load_units(uint64_t table_id, mc_svcmgr_service_unit* units) {
    char names_blob[512];
    mc_zero_bytes(names_blob, sizeof(names_blob));
    uint64_t count = 0;
    uint64_t err = mc_reg_list_values(k_cfgsrv_handle, 0, table_id, names_blob,
                                       sizeof(names_blob), &count);
    if (err != MC_REG_ERR_OK) {
        return 0;
    }

    uint32_t loaded = 0;
    uint64_t seg_start = 0;
    for (uint64_t i = 0; i <= sizeof(names_blob) && loaded < k_max_units; ++i) {
        if (i == sizeof(names_blob) || names_blob[i] == '\0') {
            uint64_t seg_len = i - seg_start;
            if (seg_len == 0) {
                if (i == sizeof(names_blob)) {
                    break;
                }
                seg_start = i + 1;
                continue;
            }
            char name[32];
            mc_zero_bytes(name, sizeof(name));
            uint64_t copy_len = seg_len < sizeof(name) - 1 ? seg_len : sizeof(name) - 1;
            for (uint64_t j = 0; j < copy_len; ++j) {
                name[j] = names_blob[seg_start + j];
            }
            uint64_t got_len = 0;
            if (mc_reg_get_binary(k_cfgsrv_handle, 0, table_id, name, &units[loaded],
                                   sizeof(mc_svcmgr_service_unit),
                                   &got_len) == MC_REG_ERR_OK &&
                got_len == sizeof(mc_svcmgr_service_unit)) {
                ++loaded;
            }
            seg_start = i + 1;
        }
    }
    return loaded;
}

// depends_on을 단순 위상정렬해 순서대로 spawn한다(ADR-196 §결정3 —
// 순환은 감지만 하고 해소하지 않는다, 걸린 유닛은 스킵+로그). 각
// 유닛의 준비완료 신호를 받은 뒤에야 다음으로 넘어간다. spawn된
// 유닛은 g_runtime에도 기록해 M42의 status/stop/restart가 찾을 수
// 있게 한다.
void start_units_in_order(mc_svcmgr_service_unit* units, uint32_t count) {
    bool started[k_max_units] = {};
    uint32_t remaining = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (units[i].scope == MC_SVCMGR_SCOPE_PER_ACCOUNT) {
            // ADR-219 §결정1/2 — per_account 유닛은 템플릿일 뿐이다.
            // enabled/depends_on을 무시하고 부팅 시 시작하지 않는다
            // (login_watcher_entry가 로그인 시점에만 인스턴스화한다).
            started[i] = true;
            continue;
        }
        ++remaining;
    }
    while (remaining > 0) {
        bool progressed = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (started[i] || !units[i].enabled) {
                continue;
            }
            bool deps_ready = true;
            for (uint32_t d = 0; d < units[i].depends_on_count; ++d) {
                bool found = false;
                for (uint32_t j = 0; j < count; ++j) {
                    if (started[j] && cstr_equals(units[j].name, units[i].depends_on[d])) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    deps_ready = false;
                    break;
                }
            }
            if (!deps_ready) {
                continue;
            }
            debug_log("[svcmgr] unit start name=");
            debug_log_n(units[i].name, mc_cstr_len(units[i].name));
            debug_log("\n");
            uint32_t thread_handle = 0;
            bool ok = spawn_unit_and_wait_ready(thread_handle);
            g_runtime_lock.lock();
            runtime_unit* rt = alloc_runtime_locked(units[i].name, "");
            if (rt != nullptr) {
                rt->running = ok;
                rt->thread_handle = thread_handle;
            }
            g_runtime_lock.unlock();
            started[i] = true;
            progressed = true;
            --remaining;
        }
        if (!progressed) {
            // 순환(또는 없는 의존)에 걸린 나머지 — 스킵하고 멈춘다.
            debug_log("[svcmgr] unit start cycle_or_missing_dep remaining>0\n");
            break;
        }
    }
}

// M43(ADR-219 §결정2) — svcmgr가 아는 유닛 템플릿 전체(부팅 시
// load_units가 채운 것 그대로) — login_watcher_entry가 로그인 시점에
// scope=PER_ACCOUNT인 것만 훑는다. 부팅 후에는 읽기만 하므로(새
// 유닛은 op_register로 g_runtime에는 들어가지만 이 배열은 갱신하지
// 않는다 — per_account 템플릿의 op_register 확장은 이번 라운드
// 범위 밖) 별도 락이 필요 없다.
mc_svcmgr_service_unit g_units[k_max_units];
uint32_t g_unit_count = 0;

// M43 — svcmgr의 첫 멀티스레드 사용(M37의 mc_thread_create). procsrv
// 는 단일 요청-응답 루프라 진짜 블로킹을 못 해(OPEN-67) svcmgr가
// 직접 폴링해야 한다 — 이 스레드가 메인 스레드의 컨트롤 프로토콜
// 처리(M42)를 막지 않도록 별도 스레드에 둔다.
[[noreturn]] void login_watcher_entry(const void*) {
    for (;;) {
        uint32_t uid = 0;
        uint64_t username_packed = 0;
        if (mc_poll_login_event(k_procsrv_handle, &uid, &username_packed) == 0) {
            mc_yield();
            continue;
        }
        char username[9] = {};
        for (uint64_t i = 0; i < 8; ++i) {
            username[i] = static_cast<char>((username_packed >> (8 * i)) & 0xFF);
        }
        debug_log("[svcmgr] login event account=");
        debug_log_n(username, mc_cstr_len(username));
        debug_log("\n");
        for (uint32_t i = 0; i < g_unit_count; ++i) {
            if (g_units[i].scope != MC_SVCMGR_SCOPE_PER_ACCOUNT) {
                continue;
            }
            uint32_t thread_handle = 0;
            uint32_t status = mc_spawn_delegated_unit(k_procsrv_handle, username_packed, &thread_handle);
            if (status != MC_PROC_STATUS_OK) {
                // 위임 없음/guest,jail 대상 — 조용히 스킵한다(ADR-218
                // §결정6 — 에러가 아니다).
                debug_log("[svcmgr] per_account spawn skip name=");
                debug_log_n(g_units[i].name, mc_cstr_len(g_units[i].name));
                debug_log(" account=");
                debug_log_n(username, mc_cstr_len(username));
                debug_log("\n");
                continue;
            }
            g_runtime_lock.lock();
            runtime_unit* rt = alloc_runtime_locked(g_units[i].name, username);
            if (rt != nullptr) {
                rt->running = true;
                rt->thread_handle = thread_handle;
            }
            g_runtime_lock.unlock();
            debug_log("[svcmgr] per_account spawn ok name=");
            debug_log_n(g_units[i].name, mc_cstr_len(g_units[i].name));
            debug_log(" account=");
            debug_log_n(username, mc_cstr_len(username));
            debug_log("\n");
        }
    }
}

// M42 — 컨트롤 프로토콜 오퍼레이션 처리. mc/svcmgr_protocol.h의
// @wire-op 마크업이 정본이다.

// procsrv/cfgsrv/vfs가 이미 쓰는 것과 같은 이유(g_io_scratch류) —
// out.pages[0].vaddr가 가리키는 메모리는 실제 sys_ipc_reply
// syscall(_start의 메인 루프, handle_list가 반환한 뒤에야 불린다)
// 시점까지 살아있어야 한다 — 이 함수의 스택 프레임은 그때 이미
// 사라졌으므로 지역 배열을 쓰면 안 된다.
char g_list_blob[256];

void handle_list(mc_message& out) {
    mc_zero_bytes(g_list_blob, sizeof(g_list_blob));
    uint64_t off = 0;
    uint32_t count = 0;
    g_runtime_lock.lock();
    for (auto& u : g_runtime) {
        if (!u.used) {
            continue;
        }
        // M43(ADR-219 §결정4) — 계정 인스턴스는 "이름@계정"으로 낸다.
        char combined[65];
        mc_zero_bytes(combined, sizeof(combined));
        uint64_t name_len = mc_cstr_len(u.name);
        for (uint64_t i = 0; i < name_len; ++i) {
            combined[i] = u.name[i];
        }
        uint64_t total_len = name_len;
        if (u.account[0] != '\0') {
            combined[total_len++] = '@';
            uint64_t acct_len = mc_cstr_len(u.account);
            for (uint64_t i = 0; i < acct_len; ++i) {
                combined[total_len++] = u.account[i];
            }
        }
        if (off + total_len + 1 >= sizeof(g_list_blob)) {
            break;
        }
        for (uint64_t i = 0; i < total_len; ++i) {
            g_list_blob[off++] = combined[i];
        }
        g_list_blob[off++] = '\0';
        ++count;
    }
    g_runtime_lock.unlock();
    out.page_count = 1;
    out.pages[0].vaddr = reinterpret_cast<uint64_t>(g_list_blob);
    out.pages[0].length = sizeof(g_list_blob);
    out.pages[0].mode = MC_TRANSFER_COPY;
    out.regs[0] = MC_SVCMGR_STATUS_OK;
    out.regs[1] = count;
}

// pages[0]의 "<유닛명>@<계정명>" 또는 "<유닛명>"(system-scope)
// 문자열을 읽어 분리한다(ADR-219 §결정4) — status/start/stop/restart
// 공용.
void read_name_account(const mc_message& in, char* out_name, char* out_account) {
    char combined[64];
    mc_zero_bytes(combined, sizeof(combined));
    const char* src = reinterpret_cast<const char*>(in.pages[0].vaddr);
    for (uint64_t i = 0; i < sizeof(combined) - 1 && src[i] != '\0'; ++i) {
        combined[i] = src[i];
    }
    split_name_account(combined, out_name, out_account);
}

void handle_status(const mc_message& in, mc_message& out) {
    char name[32];
    char account[32];
    read_name_account(in, name, account);
    g_runtime_lock.lock();
    runtime_unit* u = find_runtime_locked(name, account);
    bool running = u != nullptr && u->running;
    uint32_t thread_handle = u != nullptr ? u->thread_handle : 0;
    bool found = u != nullptr;
    g_runtime_lock.unlock();
    if (!found) {
        out.regs[0] = MC_SVCMGR_STATUS_NOT_FOUND;
        return;
    }
    out.regs[0] = MC_SVCMGR_STATUS_OK;
    out.regs[1] = running ? 1 : 0;
    out.regs[2] = thread_handle;
}

void handle_start(const mc_message& in, mc_message& out) {
    char name[32];
    char account[32];
    read_name_account(in, name, account);
    g_runtime_lock.lock();
    runtime_unit* existing = find_runtime_locked(name, account);
    bool already_running = existing != nullptr && existing->running;
    g_runtime_lock.unlock();
    if (already_running) {
        out.regs[0] = MC_SVCMGR_STATUS_ALREADY_RUNNING;
        return;
    }
    // system-scope만 svcmgr 자신의 태생적 권한으로 직접 spawn한다
    // (M40~M42와 동일). per_account 인스턴스는 op_start로 직접
    // (재)시작하지 않는다 — 로그인 시점에만 위임 확인을 거쳐
    // 인스턴스화된다(ADR-218/219) — account가 비어 있지 않으면
    // 여기서 다루지 않는다.
    if (account[0] != '\0') {
        out.regs[0] = MC_SVCMGR_STATUS_NOT_FOUND;
        return;
    }
    uint32_t thread_handle = 0;
    if (!spawn_unit_and_wait_ready(thread_handle)) {
        out.regs[0] = MC_SVCMGR_STATUS_NOT_FOUND;
        return;
    }
    g_runtime_lock.lock();
    runtime_unit* rt = alloc_runtime_locked(name, account);
    if (rt != nullptr) {
        rt->running = true;
        rt->thread_handle = thread_handle;
    }
    g_runtime_lock.unlock();
    out.regs[0] = (rt != nullptr) ? MC_SVCMGR_STATUS_OK : MC_SVCMGR_STATUS_NOT_FOUND;
}

// op_stop — 기존 sys_process_kill(ADR-178)을 그대로 쓴다(ADR-196
// §결정7 근거 — 새 종료 메커니즘을 만들지 않는다). system/per_account
// 인스턴스 둘 다 이 경로로 정지할 수 있다(kill은 대상을 가리지
// 않는다).
void handle_stop(const mc_message& in, mc_message& out) {
    char name[32];
    char account[32];
    read_name_account(in, name, account);
    g_runtime_lock.lock();
    runtime_unit* u = find_runtime_locked(name, account);
    if (u == nullptr || !u->running) {
        g_runtime_lock.unlock();
        out.regs[0] = MC_SVCMGR_STATUS_NOT_RUNNING;
        return;
    }
    uint32_t thread_handle = u->thread_handle;
    u->running = false;
    g_runtime_lock.unlock();
    do_syscall(MC_SYSCALL_PROCESS_KILL, thread_handle, 0, 0);
    out.regs[0] = MC_SVCMGR_STATUS_OK;
}

void handle_restart(const mc_message& in, mc_message& out) {
    handle_stop(in, out);  // NOT_RUNNING이어도 그냥 이어서 시작을 시도한다.
    handle_start(in, out);
}

// op_register — cfgsrv set_value를 그대로 쓴다(M41이 만든 클라이언트,
// ADR-196 §결정7 근거). table_id는 부팅 시 이미 연 것을 그대로
// 재사용한다(아래 g_services_table_id).
uint64_t g_services_table_id = 0;

void handle_register(const mc_message& in, mc_message& out) {
    if (in.page_count != 1) {
        out.regs[0] = MC_SVCMGR_STATUS_NOT_FOUND;
        return;
    }
    // kernel/core/ipc/endpoint.cpp::release_previous_ipc_mapping(ADR-159/161)
    // — 이 스레드가 mc_reg_set_binary의 nested sys_call로 다시
    // deliver_message의 목적지가 되는 순간(cfgsrv의 응답을 받을 때)
    // in.pages[0]의 매핑이 해제된다 — 그 전에 통째로 복사해 둔다
    // (mc/cfgsrv_client.c의 g_cfg_value_buf 채우기와 같은 이유).
    mc_svcmgr_service_unit unit = *reinterpret_cast<const mc_svcmgr_service_unit*>(in.pages[0].vaddr);
    uint64_t err = mc_reg_set_binary(k_cfgsrv_handle, 0, g_services_table_id, unit.name, &unit,
                                      sizeof(mc_svcmgr_service_unit));
    if (err != MC_REG_ERR_OK) {
        out.regs[0] = MC_SVCMGR_STATUS_NOT_FOUND;
        return;
    }
    // status/start가 바로 찾을 수 있게(아직 실행 중은 아님) — account=""
    // (system scope와 같은 자리, per_account 템플릿은 로그인 시점에야
    // 실제 계정 슬롯이 생긴다).
    g_runtime_lock.lock();
    alloc_runtime_locked(unit.name, "");
    g_runtime_lock.unlock();
    out.regs[0] = MC_SVCMGR_STATUS_OK;
}

void handle_unregister(const mc_message& in, mc_message& out) {
    char name[32];
    mc_zero_bytes(name, sizeof(name));
    const char* src = reinterpret_cast<const char*>(in.pages[0].vaddr);
    for (uint64_t i = 0; i < sizeof(name) - 1 && src[i] != '\0'; ++i) {
        name[i] = src[i];
    }
    uint64_t err = mc_reg_delete_value(k_cfgsrv_handle, 0, g_services_table_id, name);
    out.regs[0] = (err == MC_REG_ERR_OK) ? MC_SVCMGR_STATUS_OK : MC_SVCMGR_STATUS_NOT_FOUND;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    uint32_t self_pid = mc_getpid(k_procsrv_handle);
    debug_log(self_pid != 0 ? "[svcmgr] self_register ok=1\n" : "[svcmgr] self_register ok=0\n");

    uint32_t adopted = mc_adopt_orphans(k_procsrv_handle, self_pid);
    debug_log(adopted > 0 ? "[svcmgr] adopt_orphans ok=1\n" : "[svcmgr] adopt_orphans ok=0\n");

    // M41 — @global/system/services를 연다(없으면 만든다, uid=0/root
    // 관례 — registry-decisions.md ADR-197 §결정3의 owner).
    uint64_t table_id = 0;
    uint64_t open_err = mc_reg_open_or_create(k_cfgsrv_handle, MC_REG_OP_OPEN_TABLE, 0, "root",
                                               k_services_table_path, &table_id);
    bool freshly_created = false;
    if (open_err == MC_REG_ERR_NOT_FOUND) {
        open_err = mc_reg_open_or_create(k_cfgsrv_handle, MC_REG_OP_CREATE_TABLE, 0, "root",
                                          k_services_table_path, &table_id);
        freshly_created = (open_err == MC_REG_ERR_OK);
    }
    debug_log(open_err == MC_REG_ERR_OK ? "[svcmgr] services table open ok=1\n"
                                         : "[svcmgr] services table open ok=0\n");
    g_services_table_id = table_id;

    if (freshly_created) {
        // M42의 op_register는 이제 있지만, 부팅 시점엔 아직 아무도
        // 부를 수 없다(컨트롤 클라이언트도 svcmgr 뒤에 spawn된다) —
        // svcmgr 자신이 자기테스트 유닛 둘을 등록한다(procsrv의 M27
        // 자기테스트와 같은 정신) — svc-b가 svc-a에 depends_on.
        mc_svcmgr_service_unit unit_a{};
        mc_pack_bytes(unit_a.name, sizeof(unit_a.name), "svc-a", 5);
        unit_a.enabled = 1;
        mc_reg_set_binary(k_cfgsrv_handle, 0, table_id, "svc-a", &unit_a, sizeof(unit_a));

        mc_svcmgr_service_unit unit_b{};
        mc_pack_bytes(unit_b.name, sizeof(unit_b.name), "svc-b", 5);
        unit_b.enabled = 1;
        unit_b.depends_on_count = 1;
        mc_pack_bytes(unit_b.depends_on[0], sizeof(unit_b.depends_on[0]), "svc-a", 5);
        mc_reg_set_binary(k_cfgsrv_handle, 0, table_id, "svc-b", &unit_b, sizeof(unit_b));

        // M43(user-service-manager.md §M43, ADR-219) — per_account
        // 자기테스트 템플릿 하나("svc-u"). 계정별 서비스 위임
        // (ADR-218)이 있는 계정이 로그인할 때만 인스턴스화된다 —
        // enabled/depends_on은 이 scope에서 무시된다.
        mc_svcmgr_service_unit unit_u{};
        mc_pack_bytes(unit_u.name, sizeof(unit_u.name), "svc-u", 5);
        unit_u.scope = MC_SVCMGR_SCOPE_PER_ACCOUNT;
        mc_reg_set_binary(k_cfgsrv_handle, 0, table_id, "svc-u", &unit_u, sizeof(unit_u));
        debug_log("[svcmgr] self-test units registered ok=1\n");
    }

    g_unit_count = load_units(table_id, g_units);
    debug_log(g_unit_count > 0 ? "[svcmgr] load_units ok=1\n" : "[svcmgr] load_units ok=0\n");
    start_units_in_order(g_units, g_unit_count);

    // M41 — delete_value로 svc-b를 지우고 다시 목록을 읽어 실제로
    // 빠졌는지 확인한다("재부팅" 부분은 범위 밖, M41 done 참고).
    // svc-b의 **프로세스**는 이미 spawn돼 계속 살아있다(M42가 데모
    // 유닛을 무한 대기로 바꿔서 op_stop 대상이 필요해졌다) — 이
    // 삭제는 레지스트리 항목만 지운다, 실행 중인 프로세스와는
    // 무관하다.
    mc_reg_delete_value(k_cfgsrv_handle, 0, table_id, "svc-b");
    char names_after[512];
    mc_zero_bytes(names_after, sizeof(names_after));
    uint64_t count_after = 0;
    uint64_t list_err = mc_reg_list_values(k_cfgsrv_handle, 0, table_id, names_after,
                                            sizeof(names_after), &count_after);
    // M43 — svc-u(per_account 템플릿)가 테이블에 추가되며 남는 개수가
    // 1(svc-a)에서 2(svc-a, svc-u)로 늘었다.
    bool delete_ok = (list_err == MC_REG_ERR_OK) && (count_after == 2) &&
                      !blob_contains_name(names_after, sizeof(names_after), "svc-b");
    debug_log(delete_ok ? "[svcmgr] delete_value svc-b ok=1\n" : "[svcmgr] delete_value svc-b ok=0\n");

    // M43(ADR-219) — 로그인 감시 백그라운드 스레드(M37 mc_thread_create,
    // 이 서버의 첫 멀티스레드 사용). owner_space/handle_table을 그대로
    // 공유하므로(fork처럼 복제하지 않는다) k_procsrv_handle 등 이미
    // 열린 핸들을 그대로 쓸 수 있다.
    {
        static _Alignas(16) uint8_t watcher_stack[16384];
        uint64_t stack_top = reinterpret_cast<uint64_t>(watcher_stack) + sizeof(watcher_stack);
        stack_top &= ~0xFULL;
        uint64_t watcher_tid = 0;
        uint64_t watcher_err =
            mc_thread_create(reinterpret_cast<uint64_t>(&login_watcher_entry), stack_top, 0, 0, 0,
                              &watcher_tid);
        debug_log(watcher_err == 0 ? "[svcmgr] login watcher thread ok=1\n"
                                    : "[svcmgr] login watcher thread ok=0\n");
    }

    // ADR-192 §결정3 — 프로세스 트리의 영구 루트는 이 데몬이다.
    // M42 — 이제 own endpoint 위에서 실제 컨트롤 프로토콜을
    // 처리한다(mc/svcmgr_protocol.h).
    constexpr uint32_t k_own_endpoint_handle = 1;
    for (;;) {
        mc_message in{};
        do_syscall(MC_SYSCALL_IPC_RECV, k_own_endpoint_handle, reinterpret_cast<uint64_t>(&in), 0);
        mc_message out{};
        out.label = in.label;
        switch (in.label) {
            case MC_SVCMGR_OP_LIST:
                handle_list(out);
                break;
            case MC_SVCMGR_OP_STATUS:
                handle_status(in, out);
                break;
            case MC_SVCMGR_OP_START:
                handle_start(in, out);
                break;
            case MC_SVCMGR_OP_STOP:
                handle_stop(in, out);
                break;
            case MC_SVCMGR_OP_RESTART:
                handle_restart(in, out);
                break;
            case MC_SVCMGR_OP_REGISTER:
                handle_register(in, out);
                break;
            case MC_SVCMGR_OP_UNREGISTER:
                handle_unregister(in, out);
                break;
            default:
                break;
        }
        do_syscall(MC_SYSCALL_IPC_REPLY, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
