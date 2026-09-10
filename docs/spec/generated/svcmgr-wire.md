# svcmgr 와이어 프로토콜 (자동 생성 — 손으로 고치지 않는다)

`tools/gen-wire-docs.py`가 svcmgr_protocol.h의 `@wire-op` 마크업에서 추출했다(ADR-195). 헤더가 바뀌면 이 파일을 다시 생성한다 — 이 파일 자체를 손으로 고치지 않는다.

| label | name | request | reply | source |
|---|---|---|---|---|
| 1 | list | (없음) | uint32 status; uint32 count | svcmgr_protocol.h:46 |
| 2 | status | char name[32] | uint32 status; uint8 running; uint32 thread_handle | svcmgr_protocol.h:48 |
| 3 | start | char name[32] | uint32 status | svcmgr_protocol.h:50 |
| 4 | stop | char name[32] | uint32 status | svcmgr_protocol.h:52 |
| 5 | restart | char name[32] | uint32 status | svcmgr_protocol.h:54 |
| 6 | register | mc_svcmgr_service_unit unit (pages[0]) | uint32 status | svcmgr_protocol.h:56 |
| 7 | unregister | char name[32] | uint32 status | svcmgr_protocol.h:58 |
