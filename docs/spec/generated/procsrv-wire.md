# procsrv 와이어 프로토콜 (자동 생성 — 손으로 고치지 않는다)

`tools/gen-wire-docs.py`가 procsrv_protocol.h의 `@wire-op` 마크업에서 추출했다(ADR-195). 헤더가 바뀌면 이 파일을 다시 생성한다 — 이 파일 자체를 손으로 고치지 않는다.

| label | name | request | reply | source |
|---|---|---|---|---|
| 10 | wait | uint32 target_pid; uint32 caller_pid | uint32 status; int32 exit_code | procsrv_protocol.h:15 |
| 11 | kill | uint32 target_pid; uint32 caller_pid | uint32 status | procsrv_protocol.h:17 |
| 12 | exit_report | uint32 pid; int32 exit_code | (없음) | procsrv_protocol.h:19 |
| 13 | self_register | (없음) | uint32 status; uint32 pid | procsrv_protocol.h:31 |
| 14 | fork_register | uint32 caller_pid | uint32 status; uint32 new_pid | procsrv_protocol.h:33 |
| 15 | adopt_orphans | uint32 caller_pid | uint32 status; uint32 adopted_count | procsrv_protocol.h:44 |
| 16 | poll_login_event | (없음) | uint32 status; uint32 uid; uint64 username_packed | procsrv_protocol.h:70 |
| 17 | spawn_delegated_unit | uint64 username_packed | uint32 status; uint32 out_thread_handle | procsrv_protocol.h:87 |
