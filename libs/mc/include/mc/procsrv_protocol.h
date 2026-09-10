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

// wait/kill 응답 regs[0].
#define MC_PROC_STATUS_OK 0u
#define MC_PROC_STATUS_NOT_FOUND 1u
#define MC_PROC_STATUS_STILL_RUNNING 2u

#endif  // MC_PROCSRV_PROTOCOL_H
