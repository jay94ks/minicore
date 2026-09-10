// userland/svcmgr-ctl-test/main.c — M42(user-service-manager.md §M42)
// "새 컨트롤 클라이언트"(계획 문서가 "별도 최소 테스트 프로그램
// 또는 셸의 새 빌트인, 착수 시점에 확정"이라 남긴 자리 — 별도
// 최소 테스트 프로그램으로 확정했다, 셸을 건드리지 않고 격리해
// 검증할 수 있다). svcmgr의 컨트롤 프로토콜(mc/svcmgr_protocol.h)
// 을 실제로 호출해 계획의 검증 목표를 그대로 재현한다: op_stop으로
// 실행 중인 데모 서비스(svc-a, M41이 부팅 시 이미 띄워 둔 것)를
// 정지시키고 op_status로 확인한 뒤, op_start로 다시 띄워 준비완료
// 왕복이 다시 성립함을 확인한다. op_register로 새 유닛(svc-c)을
// 추가한 뒤, cfgsrv에 직접(M41의 mc/cfgsrv_client.h) 물어 실제로
// 등록됐는지 확인한다 — 계획 원문의 "재부팅해 확인"은 cfgsrv의
// 저장 파일이 기본 memfs라 진짜 재부팅을 못 버텨(M41 done 참고)
// 같은 부팅 안에서의 직접 확인으로 좁혔다.
//
// libk+libmc만 링크한 순수 minicore 네이티브 실행파일이다(svcmgr
// 자신과 같은 이유).
#include <mc/cfgsrv_client.h>
#include <mc/svcmgr_protocol.h>
#include <mc/syscall.h>
#include <mc/util.h>

// servers/CMakeLists.txt의 --depends=svcmgr-ctl-test:svcmgr,cfgsrv
// 순서 그대로 handle 2/3.
#define K_SVCMGR_HANDLE 2u
#define K_CFGSRV_HANDLE 3u
#define K_SERVICES_TABLE_PATH "@global/system/services"

static void debug_log(const char* msg) {
    mc_raw_syscall(MC_SYSCALL_DEBUG_LOG, (uint64_t)(uintptr_t)msg, mc_cstr_len(msg), 0);
}

// mc/cfgsrv_client.c와 같은 이유(kernel/core/ipc/endpoint.cpp가
// page_descriptor를 4096바이트 배수·정렬로 강제한다) — 이 파일 전용
// 정적 페이지 버퍼.
_Alignas(4096) static char g_name_buf[4096];
_Alignas(4096) static mc_svcmgr_service_unit g_unit_buf;  // sizeof < 4096, 한 페이지에 맞음.

static uint64_t call_by_name(uint32_t op, const char* name, mc_message* reply) {
    mc_zero_bytes(g_name_buf, sizeof(g_name_buf));
    mc_pack_bytes(g_name_buf, sizeof(g_name_buf), name, mc_cstr_len(name));
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = op;
    req.page_count = 1;
    req.pages[0].vaddr = (uint64_t)(uintptr_t)g_name_buf;
    req.pages[0].length = sizeof(g_name_buf);
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_zero_bytes(reply, sizeof(*reply));
    mc_ipc_call(K_SVCMGR_HANDLE, &req, reply);
    return reply->regs[0];
}

void mc_main(const void* argv_or_null) {
    (void)argv_or_null;

    mc_message reply;
    uint64_t status = call_by_name(MC_SVCMGR_OP_STATUS, "svc-a", &reply);
    int running_before = (status == MC_SVCMGR_STATUS_OK) && (reply.regs[1] == 1);
    debug_log(running_before ? "[svcmgr-ctl-test] status svc-a running=1\n"
                              : "[svcmgr-ctl-test] status svc-a running=0\n");

    status = call_by_name(MC_SVCMGR_OP_STOP, "svc-a", &reply);
    debug_log(status == MC_SVCMGR_STATUS_OK ? "[svcmgr-ctl-test] stop svc-a ok=1\n"
                                             : "[svcmgr-ctl-test] stop svc-a ok=0\n");

    status = call_by_name(MC_SVCMGR_OP_STATUS, "svc-a", &reply);
    int stopped = (status == MC_SVCMGR_STATUS_OK) && (reply.regs[1] == 0);
    debug_log(stopped ? "[svcmgr-ctl-test] status svc-a stopped=1\n"
                       : "[svcmgr-ctl-test] status svc-a stopped=0\n");

    status = call_by_name(MC_SVCMGR_OP_START, "svc-a", &reply);
    debug_log(status == MC_SVCMGR_STATUS_OK ? "[svcmgr-ctl-test] start svc-a ok=1\n"
                                             : "[svcmgr-ctl-test] start svc-a ok=0\n");

    status = call_by_name(MC_SVCMGR_OP_STATUS, "svc-a", &reply);
    int running_again = (status == MC_SVCMGR_STATUS_OK) && (reply.regs[1] == 1);
    debug_log(running_again ? "[svcmgr-ctl-test] status svc-a running again=1\n"
                             : "[svcmgr-ctl-test] status svc-a running again=0\n");

    // op_register — svc-c, 의존 없음, enabled.
    mc_zero_bytes(&g_unit_buf, sizeof(g_unit_buf));
    mc_pack_bytes(g_unit_buf.name, sizeof(g_unit_buf.name), "svc-c", 5);
    g_unit_buf.enabled = 1;
    mc_message reg_req;
    mc_zero_bytes(&reg_req, sizeof(reg_req));
    reg_req.label = MC_SVCMGR_OP_REGISTER;
    reg_req.page_count = 1;
    reg_req.pages[0].vaddr = (uint64_t)(uintptr_t)&g_unit_buf;
    reg_req.pages[0].length = 4096;
    reg_req.pages[0].mode = MC_TRANSFER_COPY;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(K_SVCMGR_HANDLE, &reg_req, &reply);
    debug_log(reply.regs[0] == MC_SVCMGR_STATUS_OK ? "[svcmgr-ctl-test] register svc-c ok=1\n"
                                                    : "[svcmgr-ctl-test] register svc-c ok=0\n");

    // svcmgr을 거치지 않고 cfgsrv에 직접 물어 실제로 등록됐는지
    // 확인한다(M41의 클라이언트 재사용) — "재부팅해 확인"을 좁힌
    // 부분, 파일 상단 주석 참고.
    uint64_t table_id = 0;
    uint64_t open_err = mc_reg_open_or_create(K_CFGSRV_HANDLE, MC_REG_OP_OPEN_TABLE, 0, "root",
                                               K_SERVICES_TABLE_PATH, &table_id);
    char names_blob[512];
    mc_zero_bytes(names_blob, sizeof(names_blob));
    uint64_t count = 0;
    int found_svc_c = 0;
    if (open_err == MC_REG_ERR_OK) {
        uint64_t list_err =
            mc_reg_list_values(K_CFGSRV_HANDLE, 0, table_id, names_blob, sizeof(names_blob), &count);
        if (list_err == MC_REG_ERR_OK) {
            uint64_t seg_start = 0;
            for (uint64_t i = 0; i <= sizeof(names_blob); ++i) {
                if (i == sizeof(names_blob) || names_blob[i] == '\0') {
                    uint64_t seg_len = i - seg_start;
                    if (seg_len == 5 && mc_bytes_equal(names_blob + seg_start, "svc-c", 5)) {
                        found_svc_c = 1;
                    }
                    seg_start = i + 1;
                }
            }
        }
    }
    debug_log(found_svc_c ? "[svcmgr-ctl-test] cfgsrv sees svc-c ok=1\n"
                           : "[svcmgr-ctl-test] cfgsrv sees svc-c ok=0\n");

    // M43(user-service-manager.md §M43, docs/design/boot-and-drivers.md
    // ADR-219) — 계정별 유저 서비스 인스턴스 검증. login이 test/root
    // 둘 다 로그인시켰고 user-service-delegate-test가 둘 다 위임을
    // 등록해 뒀으니(servers/CMakeLists.txt의 순서가 보장), svcmgr의
    // 로그인 감시 스레드가 "svc-u"(per_account 템플릿) 인스턴스를
    // 계정마다 하나씩 spawn했어야 한다. 그 스레드는 svcmgr의 메인
    // IPC 루프와 별개로 돌아 정확한 완료 시점을 모르므로, 짧게
    // 재시도한다(procsrv의 기존 폴링 자기테스트와 같은 요령).
    uint32_t test_thread_handle = 0;
    uint32_t root_thread_handle = 0;
    int test_running = 0;
    int root_running = 0;
    for (int attempt = 0; attempt < 64 && !(test_running && root_running); ++attempt) {
        if (!test_running) {
            status = call_by_name(MC_SVCMGR_OP_STATUS, "svc-u@test", &reply);
            if (status == MC_SVCMGR_STATUS_OK && reply.regs[1] == 1) {
                test_running = 1;
                test_thread_handle = (uint32_t)reply.regs[2];
            }
        }
        if (!root_running) {
            status = call_by_name(MC_SVCMGR_OP_STATUS, "svc-u@root", &reply);
            if (status == MC_SVCMGR_STATUS_OK && reply.regs[1] == 1) {
                root_running = 1;
                root_thread_handle = (uint32_t)reply.regs[2];
            }
        }
        if (!(test_running && root_running)) {
            mc_yield();
        }
    }
    debug_log(test_running ? "[svcmgr-ctl-test] status svc-u@test running=1\n"
                            : "[svcmgr-ctl-test] status svc-u@test running=0\n");
    debug_log(root_running ? "[svcmgr-ctl-test] status svc-u@root running=1\n"
                           : "[svcmgr-ctl-test] status svc-u@root running=0\n");
    int distinct_instances =
        test_running && root_running && test_thread_handle != root_thread_handle;
    debug_log(distinct_instances ? "[svcmgr-ctl-test] per-account instances distinct=1\n"
                                  : "[svcmgr-ctl-test] per-account instances distinct=0\n");
}
