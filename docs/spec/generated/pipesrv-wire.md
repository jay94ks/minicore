# pipesrv 와이어 프로토콜 (자동 생성 — 손으로 고치지 않는다)

`tools/gen-wire-docs.py`가 pipesrv_protocol.h의 `@wire-op` 마크업에서 추출했다(ADR-195). 헤더가 바뀌면 이 파일을 다시 생성한다 — 이 파일 자체를 손으로 고치지 않는다.

| label | name | request | reply | source |
|---|---|---|---|---|
| 1 | create | (없음) | uint32 status; uint64 read_id; uint64 write_id | pipesrv_protocol.h:27 |
| 2 | read | uint64 id; uint64 requested_len | uint32 status; uint64 len | pipesrv_protocol.h:32 |
| 3 | write | uint64 id; uint64 len | uint32 status; uint64 written | pipesrv_protocol.h:37 |
| 4 | close | uint64 id | uint32 status | pipesrv_protocol.h:41 |
| 5 | dup | uint64 id | uint32 status | pipesrv_protocol.h:50 |
