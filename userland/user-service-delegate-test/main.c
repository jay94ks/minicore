// userland/user-service-delegate-test/main.c — user-service-manager.md
// §M43(docs/design/security-model.md ADR-218) 검증용 최소 테스트
// 프로그램. "test"/"root" 두 계정이 각각 svcmgr에게 "내 몫의 유저
// 서비스만" 영구 위임을 등록한다 — 자가서비스 grant(ADR-218 §결정2,
// 새 오퍼레이션 없이 cfgsrv의 기존 set_value를 그 계정 자신의
// caller_uid로 직접 부른다). svcmgr가 부팅 시 마지막으로 spawn되고
// login이 그 전에 두 계정을 로그인시키므로(servers/login/main.cpp),
// 이 프로그램은 반드시 login 이후·svcmgr 이전에 실행돼야 위임이
// login 이벤트 처리 시점(svcmgr의 로그인 감시 스레드, ADR-219)보다
// 먼저 자리 잡는다(servers/CMakeLists.txt의 배치 순서 참고).
//
// libk+libmc만 링크한 순수 minicore 네이티브 실행파일이다(svcmgr
// 자신과 같은 이유).
#include <mc/cfgsrv_client.h>
#include <mc/syscall.h>
#include <mc/util.h>

// servers/CMakeLists.txt의 --depends=user-service-delegate-test:cfgsrv
// 순서 그대로 handle 2(handle 1은 initrun이 모든 서비스에 기본으로
// 만들어 주는 own endpoint — svcmgr-ctl-test와 같은 관례).
#define K_CFGSRV_HANDLE 2u

static void debug_log(const char* msg) {
    mc_raw_syscall(MC_SYSCALL_DEBUG_LOG, (uint64_t)(uintptr_t)msg, mc_cstr_len(msg), 0);
}

// 실행 중 발견(2026-09-10, servers/procsrv/main.cpp::
// check_service_delegation의 같은 주석 참고) — "@global/..." 경로는
// cfgsrv가 스키마를 항상 "global"로 고정 취급해 caller_uid!=0인
// 계정의 CREATE_TABLE을 절대 통과시키지 않는다. 계정 자신의 스키마
// (@<계정명>/...)를 써야 자가서비스 grant(ADR-218 §결정2)가
// 성립한다.
//
// ADR-218 §1 — 값은 {u64 granted_at, u8 mode}뿐이다. granted_at은
// 이 라운드에선 검증에 쓰이지 않아 0으로 둔다(YAGNI — 실시간
// 시계 접근이 없다). mode=0(permanent)만 v1이 지원한다.
static uint64_t grant_service_delegation(uint32_t uid, const char* username) {
    char path[64];
    mc_zero_bytes(path, sizeof(path));
    uint64_t p = 0;
    path[p++] = '@';
    uint64_t name_len = mc_cstr_len(username);
    for (uint64_t i = 0; i < name_len; ++i) {
        path[p + i] = username[i];
    }
    p += name_len;
    const char* suffix = "/system/service-delegate";
    uint64_t suffix_len = mc_cstr_len(suffix);
    for (uint64_t i = 0; i < suffix_len; ++i) {
        path[p + i] = suffix[i];
    }

    uint64_t table_id = 0;
    uint64_t err =
        mc_reg_open_or_create(K_CFGSRV_HANDLE, MC_REG_OP_OPEN_TABLE, uid, username, path, &table_id);
    if (err == MC_REG_ERR_NOT_FOUND) {
        err = mc_reg_open_or_create(K_CFGSRV_HANDLE, MC_REG_OP_CREATE_TABLE, uid, username, path,
                                     &table_id);
    }
    if (err != MC_REG_ERR_OK) {
        return err;
    }

    uint8_t entry[9];
    mc_zero_bytes(entry, sizeof(entry));
    entry[8] = 0;  // mode=permanent.
    return mc_reg_set_binary(K_CFGSRV_HANDLE, uid, table_id, "svcmgr", entry, sizeof(entry));
}

void mc_main(const void* argv_or_null) {
    (void)argv_or_null;

    uint64_t test_ok = grant_service_delegation(1000, "test");
    debug_log(test_ok == MC_REG_ERR_OK ? "[user-service-delegate-test] grant test ok=1\n"
                                        : "[user-service-delegate-test] grant test ok=0\n");

    uint64_t root_ok = grant_service_delegation(0, "root");
    debug_log(root_ok == MC_REG_ERR_OK ? "[user-service-delegate-test] grant root ok=1\n"
                                        : "[user-service-delegate-test] grant root ok=0\n");
}
