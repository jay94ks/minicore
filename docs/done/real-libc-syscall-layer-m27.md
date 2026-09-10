# 완료 보고: real-libc-syscall-layer M27 — procsrv 실제 프로세스 테이블 + 범용 OP_WAIT/OP_KILL

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M27
**관련 결정**: [security-model.md](../design/security-model.md) ADR-201,
[build-system.md](../design/build-system.md) ADR-195
**실행일**: 2026-09-10

## 완료한 것

1. `tools/gen-wire-docs.py`(신규) — ADR-195의 `@wire-op` 마크업을
   정규식으로 추출해 `docs/spec/generated/<server>-wire.md`를
   생성한다. label 중복을 오류로 잡는다. procsrv 프로토콜 헤더가
   이 도구의 첫 실전 적용 사례다.
2. `libs/mc/include/mc/procsrv_protocol.h`(신규) — procsrv 프로토콜의
   정본. `@wire-op` 마크업과 함께 `MC_PROC_OP_WAIT`(10)/
   `MC_PROC_OP_KILL`(11)/`MC_PROC_OP_EXIT_REPORT`(12)를 정의한다.
   `docs/spec/generated/procsrv-wire.md`를 생성해 확인했다(OPEN-54
   해소).
3. `servers/procsrv/main.cpp`에 실제 `process_entry` 테이블(pid/
   parent_pid/thread_handle/state/exit_code)을 추가했다 — procsrv
   자기 자신이 pid=1로 부트스트랩 등록된다. `alloc_pid()`/
   `find_process()`/`insert_process_entry()`/`reparent_children()`
   헬퍼와 `handle_proc_wait`/`handle_proc_kill`/`handle_proc_exit_report`
   핸들러를 procsrv의 기존 서비스 루프(`k_own_endpoint_handle`)에
   추가했다.
4. 새 자기테스트 `run_general_process_table_test()` — procsrv가 세
   프로세스 C(kill 대상, 기존 M22의 `kill_target_argv` 재사용)→
   B(자식, `proc_op::exit_report`로 스스로 종료를 보고)→A(부모, B/C의
   pid를 이미 알고 `proc_op::wait`/`kill`을 실제로 호출)를 순서대로
   스폰한다. M22의 자기테스트와 달리 **procsrv 자신의 전용
   endpoint가 아니라 pid로 식별되는 임의의 두 유저 프로세스** 사이의
   왕복이다 — A가 B의 pid로 `wait`해 exit_code(77)를 정확히 회수하고
   ("[procsrv] m27 wait exit_code ok=1"), C의 pid로 `kill`을 요청해
   성공을 확인한다("[procsrv] m27 kill ok=1").
5. 새 자기테스트 `run_reparenting_mechanism_test()` — ADR-192
   §결정3의 재부모화 메커니즘(`reparent_children(from, to)`, 대상은
   매개변수)을 합성 pid로 증명한다("[procsrv] reparent mechanism
   ok=1").
6. `docs/design/security-model.md` ADR-201 — 이번 라운드의 범위
   축소 결정(자기주장 caller_pid, 비블로킹 폴링 wait, 재부모화
   메커니즘과 실제 트리거의 분리)을 기록했다. `docs/spec/procsrv.md`
   §2/§6에 이 ADR을 가리키는 실제 구현 각주를 추가했다.
7. `docs/design/open-items.md` — OPEN-54를 해결된 항목으로 옮기고,
   OPEN-64를 범위 좁혀 남은 부분(`dup_for_new_client` fd 진실
   공급원)만 유지했다. 새 OPEN-67(자기주장 pid 미검증 + 비블로킹
   폴링 wait)을 열었다.

## 실제로 겪은 문제 — parent_pid 불일치로 wait가 항상 NOT_FOUND

첫 QEMU 실행에서 `m27 wait exit_code ok=0`으로 실패했다(kill/reparent는
성공). 원인: `run_general_process_table_test()`가 B/C를
`insert_process_entry(..., k_procsrv_self_pid, ...)`로 등록했다(물리적
스폰자가 procsrv 자신이므로) — 그런데 A는 `handle_proc_wait`의 권한
검사(`target->parent_pid == caller_pid`)에서 **자기 자신의 pid**를
`caller_pid`로 보낸다. B의 `parent_pid`가 procsrv(pid=1)로 등록돼
있어 A의 pid와 절대 일치하지 않아, `wait`가 매번(재시도 64회 내내)
`MC_PROC_STATUS_NOT_FOUND`만 반환했다 — B가 실제로 exit_report를
보냈는지와 무관하게 항상 실패였다.

수정: A의 pid를 **먼저 할당**(`alloc_pid()`)해 두고, C/B를 등록할
때 그 pid를 `parent_pid`로 쓰도록 순서를 바꿨다("물리적으로 누가
스폰했는가"와 "테이블상 부모가 누구인가"가 다른 것은 procsrv가
A/B/C 전부를 대신 스폰해 주는 이번 라운드의 단순화 때문이라고
`run_general_process_table_test()`에 주석으로 남겼다). 재빌드 후
재실행해 즉시 통과를 확인했다.

## 의도적으로 좁힌 범위 (ADR-201)

- **caller_pid는 자기주장 값이다** — 커널 badge로 검증하지 않는다.
  procsrv.md §3의 원래 설계(badge로 발신자 신원을 확인)를 실제로
  구현하려면 badge에 pid를 인코딩하는 커널 캐패빌리티 확장이
  필요한데, 이번 라운드는 그 확장 없이 "임의의 두 프로세스 사이의
  wait/kill이 실제로 동작"이라는 핵심만 증명했다.
- **wait는 블로킹이 아니라 비블로킹 폴링이다** — procsrv는 단일
  요청-응답 루프(`sys_ipc_recv` 한 번에 메시지 하나)라 Call을 붙들고
  기다릴 수 없다. 대상이 아직 zombie가 아니면 즉시
  `MC_PROC_STATUS_STILL_RUNNING`을 반환하고 호출자가 재시도한다.
- **범용 "procsrv에게 스폰을 대행시키는" API는 만들지 않았다** —
  procsrv.md §3의 `op_fork` 설계(다른 프로세스가 IPC로 fork를
  요청)는 실제 `sys_fork`(호출 스레드 자신의 컨텍스트를 복제하는
  syscall)와 근본적으로 맞지 않는다(ADR-201 §배경). procsrv는 이번
  라운드에도 자기 자신이 직접 `sys_process_spawn`을 호출해 A/B/C를
  만든다.
- **ADR-193의 준비완료 신호(`mc_signal_ready`/`mc_wait_ready`)를
  기존 14개 서버+initrun의 boot 스폰 시퀀스에 적용하지 않았다** —
  이번 라운드는 procsrv의 process_entry 테이블 자체에 집중했고, 이미
  안정적으로 동작하는 boot 시퀀스에 불필요한 변경을 넣지 않기 위해
  의도적으로 미뤘다(user-service-manager.md M40이 이 관례를 다시
  요구하므로 그때 재검토).
- **재부모화 메커니즘은 실제 initrun 종료 이벤트와 연결하지 않았다**
  — `reparent_children()` 함수 자체는 실제로 호출 가능하고 동작이
  증명됐지만, initrun이 실제로 사라질 때 procsrv에게 알리는 새 IPC는
  만들지 않았다(그러려면 initrun이 스폰한 모든 커널 서버의
  thread_handle을 procsrv에게 넘기는 별도 등록 절차가 먼저 필요하다
  — 범위가 커서 이번 라운드에서 제외).
- OPEN-64가 지적한 `dup_for_new_client`(완전한 fd 진실 공급원
  프로토콜)는 여전히 미구현이다(원래 계획도 이 부분은 범위 밖으로
  명시했다).

## 검증

x86_64 재빌드 성공(procsrv만 변경). bootdisk 재생성. QEMU 5개
회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0) — 신규 3개 확인 문자열(m27 wait/kill/reparent) 포함 |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0) |

## 남겨 둔 것

real-libc-syscall-layer.md M28(musl syscall 번역 계층 착수)부터
계속 진행한다. OPEN-67(자기주장 pid, 비블로킹 wait)은 M32(musl의
`SYS_wait4`/`SYS_getpid` 번역) 착수 시점에 재검토 대상.
