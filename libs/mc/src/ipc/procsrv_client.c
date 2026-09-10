#include <mc/procsrv_client.h>

#include <mc/procsrv_protocol.h>
#include <mc/syscall.h>
#include <mc/util.h>

// M32(real-libc-syscall-layer.md §M32) — 이 프로세스의 pid는 유저랜드
// static 변수가 아니라 커널 스레드 객체(kernel_objects.hpp::thread::
// procsrv_pid, mc_procsrv_pid_get/set)에 캐시한다. 처음엔 이 파일도
// 유저랜드 static을 썼지만, execve()가 owner_space(BSS/데이터
// 포함)를 통째로 새 이미지로 갈아엎어 그 static을 지워버린다는 걸
// 발견해 고쳤다 — fork()의 자식이 곧바로 execve()하면(이 계획의
// 실제 검증 시나리오) 새 이미지의 procsrv_client.c는 완전히 새로
// 시작된 것처럼 보여, 자기 pid를 procsrv에 exit_report할 방법이
// 없어진다. exec()을 거쳐도 그대로인 커널 스레드 객체에 저장해야
// "실제 Linux에서 pid가 execve() 이후에도 유지된다"는 성질을 이
// 커널에서도 재현할 수 있다(kernel_objects.hpp::thread::procsrv_pid
// 주석에 더 자세히 적어 뒀다).
uint32_t mc_getpid_cached(void) {
    return mc_procsrv_pid_get();
}

uint32_t mc_getpid(uint32_t procsrv_handle) {
    uint32_t cached = mc_procsrv_pid_get();
    if (cached != 0) {
        return cached;
    }
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_PROC_OP_SELF_REGISTER;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(procsrv_handle, &req, &reply);
    if (reply.regs[0] != MC_PROC_STATUS_OK) {
        return 0;
    }
    uint32_t pid = (uint32_t)reply.regs[1];
    mc_procsrv_pid_set(pid);
    return pid;
}

long mc_fork(uint32_t procsrv_handle) {
    uint32_t self_pid = mc_getpid(procsrv_handle);

    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_PROC_OP_FORK_REGISTER;
    req.regs[0] = self_pid;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(procsrv_handle, &req, &reply);
    if (reply.regs[0] != MC_PROC_STATUS_OK) {
        return -12;  // ENOMEM 상당 — g_processes(k_max_processes=64)가 가득 찼다.
    }
    // child_pid는 아래 sys_fork(COW) 이전에 확정한 지역변수라, 곧이어
    // 만들어질 자식의 스택에도 그대로 복제된다 — 부모/자식이 별도
    // 조율 없이 같은 값을 본다(servers/procsrv/main.cpp::
    // handle_proc_fork_register() 주석 참고). 단, 자식의 **커널 스레드
    // 객체**는 create_forked_thread()가 새로 만든 것이라
    // procsrv_pid=0부터 시작한다 — 아래에서 명시적으로 채워 줘야 한다.
    uint32_t child_pid = (uint32_t)reply.regs[1];

    uint64_t raw = mc_raw_syscall(MC_SYSCALL_FORK, 0, 0, 0);
    if (raw == 0) {
        // 자식 분기 — 이 스레드(자식) 자신의 procsrv_pid를 채운다.
        mc_procsrv_pid_set(child_pid);
        return 0;
    }
    if (raw == 1) {
        return (long)child_pid;
    }
    // mc/syscall.h::process_spawn_error(실패 코드) — sys_fork 자체가
    // 실패했다. procsrv에 남은 child_pid 등록(위에서 이미 확정)은
    // 회수하지 않는다(쓰이지 않는 좀비 엔트리 하나가 남는 것뿐 —
    // 이 라운드는 그 정리까지 다루지 않는다).
    return -12;
}

uint32_t mc_wait(uint32_t procsrv_handle, uint32_t target_pid, int32_t* out_exit_code) {
    uint32_t self_pid = mc_getpid(procsrv_handle);
    // M27의 OP_WAIT는 논블로킹 폴링이다(procsrv가 단일 요청/응답
    // 루프이기 때문 — ADR-201/OPEN-67) — servers/procsrv/main.cpp::
    // run_as_m27_parent_a()가 쓰는 것과 같은 재시도 요령을 여기서도
    // 쓴다. 자식이 execve() 이후 실제로 일할 시간이 필요할 수 있어
    // M27 자기테스트(64회)보다 넉넉한 한도를 둔다.
    for (uint32_t attempt = 0; attempt < 200000u; ++attempt) {
        mc_message req;
        mc_zero_bytes(&req, sizeof(req));
        req.label = MC_PROC_OP_WAIT;
        req.regs[0] = target_pid;
        req.regs[1] = self_pid;
        mc_message reply;
        mc_zero_bytes(&reply, sizeof(reply));
        mc_ipc_call(procsrv_handle, &req, &reply);
        if (reply.regs[0] == MC_PROC_STATUS_OK) {
            if (out_exit_code != 0) {
                *out_exit_code = (int32_t)reply.regs[1];
            }
            return MC_PROC_STATUS_OK;
        }
        if (reply.regs[0] == MC_PROC_STATUS_NOT_FOUND) {
            return MC_PROC_STATUS_NOT_FOUND;
        }
        // MC_PROC_STATUS_STILL_RUNNING — 재시도.
    }
    return MC_PROC_STATUS_STILL_RUNNING;
}

void mc_process_exit_report(uint32_t procsrv_handle, uint32_t pid, int32_t exit_code) {
    if (pid == 0) {
        return;  // 이 프로세스가 procsrv에 한 번도 등록된 적 없다 — 보고할 것이 없다.
    }
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_PROC_OP_EXIT_REPORT;
    req.regs[0] = pid;
    req.regs[1] = (uint64_t)(int64_t)exit_code;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(procsrv_handle, &req, &reply);
}
