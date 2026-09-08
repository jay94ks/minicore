# 완료 보고: kernel-bootstrap M6 — IPC: endpoint + Call/Reply

**대상 계획**: [kernel-bootstrap.md](../plan/kernel-bootstrap.md) §M6
**관련 스펙**: [ipc.md](../spec/ipc.md) §2~5
**관련 결정**: ADR-002, 004, 011, 013, 023, 028, 029, 032, 042
**실행일**: 2026-09-08

## 완료 기준 달성 확인

M6의 목표는 명시적이다: "커널 스레드 2개 사이에 Call → Recv → Reply
왕복 성공." QEMU 실행으로 확인했다.

```
cmake --preset x86_64-clang -S . -B build/x86_64-clang
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh
# => 22개 항목 모두 PASS (M1~M5 20개 + M6 2개)
```

실제 QEMU 출력(발췌):

```
[ipc] setup ok=1
[sched] thread A iteration 0
[sched] thread B iteration 0
[sched] thread A iteration 1
[sched] thread B iteration 1
[ipc] server sys_recv ok=1 badge=0xcafe (expect 0xcafe) label=0x1234 regs0=41
[ipc] server sys_reply sent
[sched] thread A iteration 2
[sched] thread B iteration 2
[ipc] client sys_call ok=1 reply_label=0x5eed (expect 0x5eed) reply_regs0=42 (expect 42)
```

## 수행한 작업

### 1. kernel/core/ipc — Call/Reply + 도네이션 (ipc.md §3~5)

- [kernel/core/ipc/message.hpp](../../kernel/core/ipc/message.hpp) —
  `message`(label+regs만, M7까지 page_descriptor/handle_transfer
  없음), `ipc_error`.
- [kernel/core/ipc/endpoint.hpp](../../kernel/core/ipc/endpoint.hpp)/[.cpp](../../kernel/core/ipc/endpoint.cpp) —
  `sys_call`/`sys_recv`/`sys_reply`. handle_table을 통해 실제
  handle(§3의 `sys_call(handle, ...)` 시그니처 그대로)을 검증하고
  (`invalid_handle`/`wrong_object_type`/`permission_denied`), 대상이
  `object_kind::endpoint`이고 필요한 rights(`CAN_SEND`/`CAN_RECV`,
  ADR-029)를 가졌는지 확인한다.
- `object::endpoint`를 `kernel/core/object/kernel_objects.hpp`에
  추가했다(thread/address_space와 같은 파일 — "objects.md가 다루는
  커널 객체"라는 공통점으로 묶었다) — `waiting_servers`/`waiting_callers`
  두 개의 `intrusive_list<thread>`(M4의 `ipc_wait_hook` 재사용, 한
  스레드가 둘 중 최대 하나에만 동시에 속한다).
- `object::thread`에 `ipc_state`(대기 중인 메시지/배지/응답 목적지,
  도네이션 복원용 저장 우선순위)를 추가했다 — `sys_reply`가 handle
  없이 "가장 최근 sys_recv로 받은 호출"에 답하는 스펙 규칙(ipc.md §3)
  자체가 스레드별 상태를 요구한다.
- **도네이션(ipc.md §5, ADR-028)을 두 방향 모두 구현했다** — sys_call이
  이미 대기 중인 서버를 찾아 즉시 핸드오프하는 경우(§5 1단계)와,
  sys_recv가 이미 대기 중인 호출자를 찾아 즉시 페어링하는 경우(스펙이
  명시적으로 나누지 않은 대칭 케이스) 둘 다에서 서버가 호출자의
  `boost_level`을 일시 상속한다. `sys_reply`가 저장해 둔 원래
  값으로 복원한다(§5 3단계).

### 2. kernel/core/sched — sched::block() 추가

[kernel/core/sched/scheduler.hpp](../../kernel/core/sched/scheduler.hpp)/[.cpp](../../kernel/core/sched/scheduler.cpp)에
`block()`을 추가했다 — `yield()`와 달리 현재 스레드를 run_queue에
다시 넣지 않고 전환만 한다. `sys_call`(응답 대기)과 `sys_recv`(호출
대기)의 블로킹은 이 함수를 쓴다 — 블록된 스레드를 다시 깨우는
책임(`sched::enqueue()` 호출)은 상대방 IPC 함수가 진다.

## 실행 중 발견해 고친 버그

**데모 스레드 설계 버그(라이브러리 코드 버그 아님)**: 서버 스레드가
`sys_reply` 직후 곧바로 무한 `hlt` 루프로 들어가도록 처음 작성했더니,
`sys_reply`가 클라이언트를 run_queue에 다시 넣어도 **아무도 다시
yield()하지 않아 클라이언트가 영영 스케줄되지 않는** 현상이 실제로
재현됐다(QEMU 로그가 "server sys_reply sent"에서 완전히 멈춤 — 타임아웃을
20초로 늘려도 동일해 타이밍 문제가 아니라 진짜 정지임을 확인). M5의
"thread B done이 안 보이는" 현상과 근본 원인이 같다 — 협조적
스케줄러(M5)는 누군가 계속 `yield()`해야 회전이 유지된다. 서버가
응답을 보낸 뒤 `hlt` 전에 `sched::yield()`를 한 번 호출하도록 고쳐
해결했다. `sys_call`/`sys_recv`/`sys_reply` 자체의 로직에는 버그가
없었다 — 데모 스레드가 협조적 스케줄러의 요구사항(누군가는 계속
양보해야 한다)을 지키지 않은 것이 원인이었다.

## 검증 결과 (정직하게 보고)

- **확인함**: 클라이언트(CAN_SEND 전용, badge=0xCAFE로 위임된 프록시
  핸들)가 `sys_call`로 보낸 메시지(label=0x1234, regs[0]=41)를 서버
  (CAN_RECV 전용 프록시 핸들)가 `sys_recv`로 정확히 받고, badge도
  정확히 0xCAFE로 전파됨을 확인 — 이는 objects.md §4/ipc.md §3이 말한
  "프록시로 위임된 badge가 sys_recv 반환값에 그대로 나타난다"를
  실제로 검증한 것이다.
- **확인함**: 서버가 `sys_reply`로 보낸 응답(label=0x5EED,
  regs[0]=42=41+1)이 클라이언트의 `sys_call` 반환 시 `msg_out`에
  정확히 담겨 돌아옴 — Call → Recv → Reply 전체 왕복 성공.
- **확인함**: 두 가지 페어링 순서(서버가 먼저 기다리다 나중에 호출자가
  오는 경우, 호출자가 먼저 기다리다 나중에 서버가 오는 경우) 중
  이 데모는 후자(클라이언트가 서버보다 늦게 실행됨, run_queue
  순서상 서버 C가 먼저 `sys_recv`로 블록하고 나중에 클라이언트 D가
  `sys_call`로 그 대기 중인 서버를 찾아 즉시 핸드오프)를 실행으로
  검증했다.
- **확인하지 못함**: 반대 순서(호출자가 먼저 `sys_call`로 블록해
  `waiting_callers`에 들어간 뒤, 나중에 서버가 `sys_recv`로 그 대기
  중인 호출자를 찾아 즉시 페어링하는 경로 — `endpoint.cpp`의
  `sys_recv` 함수 안 `caller != nullptr` 분기)는 코드는 작성했지만
  이번 데모의 스케줄링 순서상 실행되지 않았다. 로직은
  `sys_call`의 대칭 분기와 거의 동일해 리뷰로는 확인했지만, 실행
  검증은 아니다 — 후속 마일스톤에서 스케줄링 순서를 바꿔 시연하는
  것을 고려할 만하다.
- **확인하지 못함**: 도네이션의 실제 관찰 가능한 효과 — 데모의 두
  스레드 모두 `boost_level`이 기본값 0으로 시작해 상속·복원이
  일어나도(0→0) 로그로 구별되지 않는다. 코드 경로(저장→상속→복원)는
  실행됐지만(디버거로 추적하면 값 변화를 볼 수 있을 것이다), 값
  자체가 바뀌는 것을 klog로 보여주지는 못했다 — 후속 마일스톤에서
  두 스레드의 초기 `boost_level`을 다르게 주면 관찰 가능해진다.
- **구현하지 않음(계획대로)**: `message`의 `pages[]`/`handles[]`(M7),
  체인 깊이 초과·순환 검사가 IPC 경로에서 실제로 트리거되는 시나리오
  (M4가 이미 `create_proxy` 자체는 검증했으므로 IPC 쪽에서 다시
  검증할 필요는 낮다고 판단).

## 다음 마일스톤과의 접점

- M7이 `message`에 `page_descriptor`/`handle_transfer`를 추가하고,
  `sys_call`/`sys_recv`의 핸드오프 지점(메시지 복사가 일어나는 두
  곳)에 페이지 복사·핸들 위임(M4의 `handle_table::create_proxy`
  재사용) 로직을 끼워 넣게 된다 — 지금의 함수 구조가 이미 그 확장을
  염두에 두고 있다.
- 실제 유저 프로세스(M8)가 생기면 `sys_call`/`sys_recv`가 지금처럼
  `handle_table&`를 직접 인자로 받는 대신 "현재 스레드가 속한
  프로세스의 handle_table"을 스스로 찾아야 한다 — 그 연결 지점은
  아직 없다(M4/M6 모두 "커널 컨텍스트 하나"로 단순화했다는 점 참고).
