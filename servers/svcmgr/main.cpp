// servers/svcmgr/main.cpp — 유저 서비스 관리자 데몬(user-service-manager.md
// §M40~M41, docs/design/boot-and-drivers.md ADR-192 §결정2/3, ADR-196,
// registry-decisions.md ADR-197). initrun이 "커널 서버" 전부를
// 기동한 뒤 **마지막으로** spawn하는 유일한 프로세스다
// (servers/CMakeLists.txt --depends=svcmgr:procsrv,cfgsrv) — initrun은
// 그 뒤 스스로 사라진다(ADR-131 §결정7).
//
// ADR-196 §결정1이 명시한 대로 libk+libmc만 링크한 순수 minicore
// 네이티브 서버다(musl 불필요) — procsrv/cfgsrv를 호출하는 또 하나의
// 유저 프로세스일 뿐, 새 커널/IPC 프리미티브를 하나도 추가하지
// 않는다.
//
// M40이 증명한 것(이제 재부모화만 그대로 유지):
//   1. 재부모화(ADR-192 §결정3) — svcmgr가 procsrv에게 자기 pid를
//      알려 "지금까지 parent_pid=k_parent_none으로 잠정 등록된"
//      모든 프로세스를 자신에게 재부모화하도록 요청한다.
//
// M41이 새로 증명하는 것 — 정적 데모 유닛 하드코딩을 실제
// `@global/system/services` 테이블(ADR-197)로 대체:
//   2. `mc/cfgsrv_client.h`(신규)로 그 테이블을 열고(없으면 만들고)
//      `list_values`+`get_value`(binary 타입)로 `mc_svcmgr_service_unit`
//      (mc/svcmgr_protocol.h) 레코드들을 읽는다.
//   3. M42의 `op_register`가 아직 없어(컨트롤 프로토콜은 다음
//      마일스톤) svcmgr 자신이 테이블이 비어 있을 때 자기테스트
//      유닛 둘을 등록한다 — `svc-b`가 `svc-a`에 `depends_on`.
//   4. `depends_on` 그래프를 단순 위상정렬해 시작 순서를 정하고,
//      각 유닛마다 ADR-193의 준비완료 신호(mc_wait_ready)를 받은
//      뒤에야 다음 유닛으로 넘어간다 — `svc-a`가 먼저, `svc-b`가
//      그 뒤에 시작돼야 한다.
//   5. `delete_value`로 `svc-b`를 지우고 다시 `list_values`를 불러
//      실제로 목록에서 빠졌는지 확인한다(계획 문서의 "지우고
//      재부팅해 확인" 중 "재부팅" 부분은 이번 라운드 범위 밖으로
//      좁혔다 — cfgsrv의 저장 파일이 기본적으로 memfs에 떨어져
//      (registry-decisions.md ADR-169 §결정2) 진짜 QEMU 재부팅을
//      거치면 사라진다, 실제 재부팅 간 영속성을 보려면 그 경로를
//      디스크 기반 FS로 바꿔야 하는데 이건 별개의 결정이다 — 그래서
//      "같은 부팅 안에서 등록→소비→삭제→재조회"로 메커니즘 자체만
//      증명한다).
//
// **exec_path는 아직 읽지 않는다** — `service_unit.exec_path`(VFS
// 경로) 필드는 구조체에 존재하고 저장/조회되지만, 이번 라운드는
// 등록된 유닛이 몇 개든 전부 M40과 같은 임베딩된 데모 ELF
// (userland/svcmgr-demo-unit)를 그대로 spawn한다 — 실제 VFS에서
// 서로 다른 ELF를 읽어 오는 것은 `mc/fs_client.h`에 쓰기 클라이언트
// (VFS에 그 ELF들을 미리 심어 둘 방법)까지 새로 필요해 이번 범위를
// 벗어난다고 판단했다(user-service-manager-m41.md done 참고).
#include <mc/cfgsrv_client.h>
#include <mc/lifecycle_client.h>
#include <mc/procsrv_client.h>
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

// 데모 유닛 하나를 spawn하고 준비완료까지 기다린다(위 파일 상단
// 주석 — exec_path는 아직 안 읽는다, 항상 같은 임베딩된 ELF).
bool spawn_unit_and_wait_ready() {
    mc_process_spawn_request req{};
    req.elf_data = reinterpret_cast<uint64_t>(g_svcmgr_demo_unit_elf);
    req.elf_size = g_svcmgr_demo_unit_elf_len;
    req.create_endpoint = true;
    uint64_t err = do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&req), 0, 0);
    if (err != 0) {
        return false;
    }
    mc_wait_ready(req.out_endpoint_proxy_handle);
    return true;
}

// M41(user-service-manager.md §M41) — @global/system/services 전체를
// 읽어 units[]에 채운다(최대 k_max_units개, YAGNI). 반환값은 실제로
// 읽은 유닛 수.
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
// 유닛의 준비완료 신호를 받은 뒤에야 다음으로 넘어간다.
void start_units_in_order(mc_svcmgr_service_unit* units, uint32_t count) {
    bool started[k_max_units] = {};
    for (uint32_t remaining = count; remaining > 0;) {
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
            spawn_unit_and_wait_ready();
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

    if (freshly_created) {
        // M42의 op_register가 아직 없어 svcmgr 자신이 자기테스트
        // 유닛 둘을 등록한다(procsrv의 M27 자기테스트가 실제 소비자가
        // 없을 때 스스로 A/B/C를 만든 것과 같은 정신) — svc-b가
        // svc-a에 depends_on.
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
        debug_log("[svcmgr] self-test units registered ok=1\n");
    }

    mc_svcmgr_service_unit units[k_max_units];
    uint32_t unit_count = load_units(table_id, units);
    debug_log(unit_count > 0 ? "[svcmgr] load_units ok=1\n" : "[svcmgr] load_units ok=0\n");
    start_units_in_order(units, unit_count);

    // M41 — delete_value로 svc-b를 지우고 다시 목록을 읽어 실제로
    // 빠졌는지 확인한다(위 파일 상단 주석 — "재부팅" 부분은 이번
    // 라운드 범위 밖).
    mc_reg_delete_value(k_cfgsrv_handle, 0, table_id, "svc-b");
    char names_after[512];
    mc_zero_bytes(names_after, sizeof(names_after));
    uint64_t count_after = 0;
    uint64_t list_err = mc_reg_list_values(k_cfgsrv_handle, 0, table_id, names_after,
                                            sizeof(names_after), &count_after);
    bool delete_ok = (list_err == MC_REG_ERR_OK) && (count_after == 1) &&
                      !blob_contains_name(names_after, sizeof(names_after), "svc-b");
    debug_log(delete_ok ? "[svcmgr] delete_value svc-b ok=1\n" : "[svcmgr] delete_value svc-b ok=0\n");

    // ADR-192 §결정3 — 프로세스 트리의 영구 루트는 이 데몬이다.
    // procsrv/vfs 등 다른 커널 서버와 같은 자리(own_endpoint_handle
    // 위에서 무한히 recv/reply)를 잡아 둔다 — M42가 실제 컨트롤
    // 프로토콜(op_list/op_start/op_stop/...)의 오퍼레이션 분기를
    // 여기 추가할 자리다. 아직은 어떤 label도 실제로 처리하지 않고
    // 빈 응답만 돌려준다.
    constexpr uint32_t k_own_endpoint_handle = 1;
    for (;;) {
        mc_message in{};
        do_syscall(MC_SYSCALL_IPC_RECV, k_own_endpoint_handle, reinterpret_cast<uint64_t>(&in), 0);
        mc_message out{};
        out.label = in.label;
        do_syscall(MC_SYSCALL_IPC_REPLY, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
