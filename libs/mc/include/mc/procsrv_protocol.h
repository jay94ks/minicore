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

// M40(user-service-manager.md §M40, docs/design/boot-and-drivers.md
// ADR-192 §결정3/ADR-196 §결정5) — initrun이 사라지는 시점에 svcmgr가
// 부른다. M27이 잠정적으로 parent_pid=k_parent_none으로 등록해 둔
// initrun의 고아들(커널 서버 전부)을 caller_pid(=svcmgr 자신의 pid,
// self-asserted — 다른 op들과 같은 ADR-201 모델)로 재부모화한다.
// svcmgr 자신도 이 시점에는 parent_pid=k_parent_none으로 등록돼
// 있으므로(자신도 initrun이 스폰), caller_pid==svcmgr_pid인 항목은
// 재부모화 대상에서 제외한다(자기 자신을 자기 부모로 만들지 않음).
// @wire-op label=15 name=adopt_orphans request="uint32 caller_pid" reply="uint32 status; uint32 adopted_count"
#define MC_PROC_OP_ADOPT_ORPHANS 15u

// wait/kill/self_register/fork_register 응답 regs[0].
#define MC_PROC_STATUS_OK 0u
#define MC_PROC_STATUS_NOT_FOUND 1u
#define MC_PROC_STATUS_STILL_RUNNING 2u
// M32 — self_register/fork_register 전용(g_processes[k_max_processes=64]
// 가 가득 찼을 때). wait/kill에는 나오지 않는다.
#define MC_PROC_STATUS_TABLE_FULL 3u
// M43 — spawn_delegated_unit 전용(위임 없음/guest·jail 대상/badge
// 불일치). poll_login_event 전용(큐가 비었음)에도 NOT_FOUND를 그대로
// 재사용한다.
#define MC_PROC_STATUS_PERMISSION_DENIED 4u

// M43(user-service-manager.md, docs/design/security-model.md ADR-217) —
// svcmgr가 받는 procsrv 핸들에만 스폰 시점에 이 badge가 스탬핑된다
// (init/initrun/main.cpp의 svcmgr+procsrv 조합 전용 하드코딩 특수
// 케이스). 0은 "오버라이드 없음"(대다수 --depends=)과 구분되어야
// 하므로 0이 아닌 값을 쓴다 — 값 자체엔 의미가 없다, procsrv가
// op_spawn_delegated_unit에서 이 정확한 값과 일치하는지만 확인한다.
#define MC_PROCSRV_SERVICE_DELEGATION_BADGE 1ull

// M43 — svcmgr가 로그인 이벤트를 폴링한다(ADR-218 §결정6, ADR-219).
// procsrv는 단일 요청-응답 루프라(OPEN-67) 진짜 블로킹을 못 해
// 비블로킹 폴링이다 — 큐가 비었으면 status=NOT_FOUND.
// @wire-op label=16 name=poll_login_event request=none reply="uint32 status; uint32 uid; uint64 username_packed"
#define MC_PROC_OP_POLL_LOGIN_EVENT 16u

// M43 — svcmgr가 계정별 유저 서비스 유닛을 그 계정 몫으로 spawn한다.
// 호출자는 반드시 MC_PROCSRV_SERVICE_DELEGATION_BADGE를 가진 badge로
// 불러야 한다(그 외 badge는 즉시 PERMISSION_DENIED, ADR-217) —
// 그 뒤 `@global/system/service-delegates/<username_packed>`(ADR-218)
// 에 유효한 위임이 있는지 확인한다. elf_data/elf_size는 요청에
// 실리지 않는다 — svcmgr가 넘긴 포인터는 svcmgr 자신의 주소공간을
// 가리켜 procsrv가 역참조할 수 없으므로, procsrv가 같은 데모 ELF를
// 자신의 컴파일 시점 데이터로 직접 심어 둔 것을 쓴다(exec_path는
// 여전히 범위 밖 — M41/M42와 같은 제약). ADR-193의 준비완료
// 핸드셰이크(create_endpoint)는 이번 라운드에 연결하지 않는다 —
// 그 프록시 핸들은 procsrv 자신의 테이블에 생기므로 svcmgr에게
// 넘기려면 sys_reply의 handles[] 위임(ADR-151)까지 얹어야 해서
// 범위를 넘는다(YAGNI, exec_path처럼 이번 라운드가 명시적으로
// 미루는 것 중 하나 — 신규 **OPEN-73**).
// @wire-op label=17 name=spawn_delegated_unit request="uint64 username_packed" reply="uint32 status; uint32 out_thread_handle"
#define MC_PROC_OP_SPAWN_DELEGATED_UNIT 17u

// M55(musl-userland-porting.md §M55, ADR-227) — 호출자(msh)가 자기
// 자식에게 이미 mc_signal_send()로 시그널을 직접 보낸 뒤, 그 사실을
// procsrv에게 알린다. procsrv는 fork_register된 자식의 thread_handle
// 을 원천적으로 모른다(항상 0으로 등록된다, insert_process_entry
// 호출부 참고) — 그래서 이 오퍼레이션은 MC_PROC_OP_KILL처럼 실제
// 커널 종료를 다시 시도하지 않고, target->parent_pid==caller_pid
// 소유권 검사만 거친 뒤 곧바로 zombie로 낙관적 마킹한다(MC_PROC_OP_KILL
// 이 이미 exit_code=-9로 SIGKILL을 인코딩하는 것과 같은 "음수=시그널
// 번호" 관례) — 이게 없으면 mc_wait()가 이 자식이 죽었다는 것을
// 전혀 못 배워 200,000회 폴링 예산을 다 태운다.
// @wire-op label=18 name=report_signaled request="uint32 target_pid; uint32 caller_pid; uint32 signal_number" reply="uint32 status"
#define MC_PROC_OP_REPORT_SIGNALED 18u

#endif  // MC_PROCSRV_PROTOCOL_H
