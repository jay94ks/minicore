// procsrv 프로토콜 정본 — 실제 프로세스 테이블 위의 범용
// wait/kill(docs/spec/procsrv.md §2/§6, real-libc-syscall-layer.md
// M27, OPEN-54 해소). ADR-195(docs/design/build-system.md)의
// `@wire-op` 마크업 컨벤션을 procsrv 프로토콜에 처음 적용한다.
//
// M27 범위(docs/design/security-model.md ADR-201) — 호출자는 자신의
// pid를 메시지 필드로 스스로 주장한다(커널 badge로 검증하지 않는다).
// 이는 의도적으로 좁힌 범위다 — 완전한 신원 검증은 이 라운드의
// 범위 밖이다(ADR-201 §근거).
#ifndef MC_PROCSRV_PROTOCOL_H
#define MC_PROCSRV_PROTOCOL_H

#include <stdint.h>

// @wire-op label=10 name=wait request="uint32 target_pid; uint32 caller_pid" reply="uint32 status; int32 exit_code"
#define MC_PROC_OP_WAIT 10u
// @wire-op label=11 name=kill request="uint32 target_pid; uint32 caller_pid" reply="uint32 status"
#define MC_PROC_OP_KILL 11u
// @wire-op label=12 name=exit_report request="uint32 pid; int32 exit_code" reply=none
#define MC_PROC_OP_EXIT_REPORT 12u

// M32(real-libc-syscall-layer.md §M32) — 진짜 musl fork()/getpid()가
// 필요로 하는 두 오퍼레이션. M27의 self-asserted caller_pid 모델을
// procsrv가 직접 스폰하지 않은 일반 프로세스에도 그대로 확장한다:
// 프로세스가 최초로 자신의 pid를 알아야 할 때(getpid 최초 호출)
// self_register로, fork()가 자식의 pid를 (실제 sys_fork 이전에)
// 미리 확정해야 할 때 fork_register로 procsrv에 등록한다(libmc의
// mc_fork/mc_getpid 참고 — "sys_fork보다 먼저 호출해 pid를 예약하면
// COW로 복제되는 지역변수를 통해 부모/자식이 같은 값을 본다"는
// 요령을 그 쪽에서 쓴다).
// @wire-op label=13 name=self_register request=none reply="uint32 status; uint32 pid"
#define MC_PROC_OP_SELF_REGISTER 13u
// @wire-op label=14 name=fork_register request="uint32 caller_pid" reply="uint32 status; uint32 new_pid"
#define MC_PROC_OP_FORK_REGISTER 14u

// wait/kill/self_register/fork_register 응답 regs[0].
#define MC_PROC_STATUS_OK 0u
#define MC_PROC_STATUS_NOT_FOUND 1u
#define MC_PROC_STATUS_STILL_RUNNING 2u
// M32 — self_register/fork_register 전용(g_processes[k_max_processes=64]
// 가 가득 찼을 때). wait/kill에는 나오지 않는다.
#define MC_PROC_STATUS_TABLE_FULL 3u

#endif  // MC_PROCSRV_PROTOCOL_H
