// servers/svcmgr/main.cpp — 유저 서비스 관리자 데몬(user-service-manager.md
// §M40, docs/design/boot-and-drivers.md ADR-192 §결정2/3, ADR-196).
// initrun이 "커널 서버" 전부를 기동한 뒤 **마지막으로** spawn하는
// 유일한 프로세스다(servers/CMakeLists.txt --depends=svcmgr:<커널
// 서버 전체>) — initrun은 그 뒤 스스로 사라진다(ADR-131 §결정7).
//
// ADR-196 §결정1이 명시한 대로 libk+libmc만 링크한 순수 minicore
// 네이티브 서버다(musl 불필요) — procsrv/cfgsrv를 호출하는 또 하나의
// 유저 프로세스일 뿐, 새 커널/IPC 프리미티브를 하나도 추가하지
// 않는다.
//
// M40은 두 가지만 증명한다:
//   1. 재부모화(ADR-192 §결정3) — svcmgr가 procsrv에게 자기 pid를
//      알려 "지금까지 parent_pid=k_parent_none으로 잠정 등록된"
//      모든 프로세스를 자신에게 재부모화하도록 요청한다
//      (mc_adopt_orphans, procsrv 자신도 pid=1로 항상 등록해 둔다 —
//      main.cpp의 해당 주석 참고, 실행 중 발견: 다른 커널 서버는
//      아무도 procsrv에 self_register하지 않아 이 하나가 유일한
//      실제 대상이었다).
//   2. 준비완료 신호(ADR-193) — 하드코딩된 데모 유닛 하나
//      (userland/svcmgr-demo-unit, tools/bin2c.py로 이 파일에 직접
//      심는다)를 spawn하고, 그 유닛이 mc_signal_ready()를 부를
//      때까지 mc_wait_ready()로 블록했다가 받으면 통과한다.
// 실제 유닛 레지스트리(@global/system/services)를 읽는 것은 M41
// 대상이라 아직 하드코딩이다.
#include <mc/lifecycle_client.h>
#include <mc/procsrv_client.h>
#include <mc/syscall.h>

#include "svcmgr_demo_unit_blob.h"

namespace {

// servers/CMakeLists.txt의 --depends=svcmgr:procsrv,... 순서 —
// procsrv를 목록 맨 앞에 둬서 항상 handle 2가 되도록 고정한다
// (servers/login/main.cpp의 k_procsrv_handle=4와 같은 관례, 그
// 서버는 depends=console,ps2,procsrv라 procsrv가 세 번째라 4였다).
constexpr uint32_t k_procsrv_handle = 2;

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

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    uint32_t self_pid = mc_getpid(k_procsrv_handle);
    debug_log(self_pid != 0 ? "[svcmgr] self_register ok=1\n" : "[svcmgr] self_register ok=0\n");

    uint32_t adopted = mc_adopt_orphans(k_procsrv_handle, self_pid);
    debug_log(adopted > 0 ? "[svcmgr] adopt_orphans ok=1\n" : "[svcmgr] adopt_orphans ok=0\n");

    // ADR-196 §결정2/3의 데모 유닛 — 하드코딩(M41이 실제 레지스트리로
    // 대체한다). create_endpoint=true로 이 유닛만을 위한 새
    // endpoint를 만든다(procsrv의 여러 M22/M27 자기테스트가 이미
    // 쓰는 것과 같은 패턴) — 이번엔 svcmgr가 sys_ipc_recv로 그
    // endpoint를 기다린다(ADR-193의 준비완료 신호 방향, wait와는
    // 반대).
    mc_process_spawn_request req{};
    req.elf_data = reinterpret_cast<uint64_t>(g_svcmgr_demo_unit_elf);
    req.elf_size = g_svcmgr_demo_unit_elf_len;
    req.create_endpoint = true;
    uint64_t spawn_err = do_syscall(MC_SYSCALL_PROCESS_SPAWN, reinterpret_cast<uint64_t>(&req), 0, 0);
    debug_log(spawn_err == 0 ? "[svcmgr] demo unit spawn ok=1\n" : "[svcmgr] demo unit spawn ok=0\n");

    if (spawn_err == 0) {
        mc_wait_ready(req.out_endpoint_proxy_handle);
        debug_log("[svcmgr] demo unit ready ok=1\n");
    }

    // ADR-192 §결정3 — 프로세스 트리의 영구 루트는 이 데몬이다.
    // procsrv/vfs 등 다른 커널 서버와 같은 자리(own_endpoint_handle
    // 위에서 무한히 recv/reply)를 잡아 둔다 — M42가 실제 컨트롤
    // 프로토콜(op_list/op_start/op_stop/...)의 오퍼레이션 분기를
    // 여기 추가할 자리다. 아직은 어떤 label도 실제로 처리하지 않고
    // 빈 응답만 돌려준다(자기 자신을 계속 살려 두는 것 자체가 이
    // 마일스톤의 목적).
    constexpr uint32_t k_own_endpoint_handle = 1;
    for (;;) {
        mc_message in{};
        do_syscall(MC_SYSCALL_IPC_RECV, k_own_endpoint_handle, reinterpret_cast<uint64_t>(&in), 0);
        mc_message out{};
        out.label = in.label;
        do_syscall(MC_SYSCALL_IPC_REPLY, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
