# 완료 보고: kernel-bootstrap M7 — IPC: 페이지·핸들 전달 + Notification

**대상 계획**: [kernel-bootstrap.md](../plan/kernel-bootstrap.md) §M7
**관련 스펙**: [ipc.md](../spec/ipc.md) §4, §7, [objects.md](../spec/objects.md) §4
**관련 결정**: ADR-002, 011, 013, 015, 023, 029, 032, 033, 035, 042
**실행일**: 2026-09-08

## 완료 기준 달성 확인

M7의 목표는 명시적이다: "1페이지 데이터를 두 스레드 사이에 copy
모드로 전달 성공." QEMU 실행으로 확인했다 — 페이지 전달에 더해
핸들 위임과 notification 왕복까지 같은 데모에서 함께 확인했다.

```
cmake --preset x86_64-clang -S . -B build/x86_64-clang
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh
# => 25개 항목 모두 PASS (M1~M6 22개 + M7 3개)
```

실제 QEMU 출력(전체, 반복 실행해도 바이트 단위로 동일):

```
[ipc2] setup ok=1
[sched] thread A iteration 0
[sched] thread B iteration 0
[ipc2] receiver sys_recv ok=1 page_count=1 content_ok=1 handle_count=1 received_handle_kind=3 (expect notification=3)
[ipc2] notifier sys_notify ok=1
[sched] thread A iteration 1
[sched] thread B iteration 1
[ipc] server sys_recv ok=1 badge=0xcafe (expect 0xcafe) label=0x1234 regs0=41
[ipc] server sys_reply sent
[ipc2] sender sys_call ok=1 ack_label=0xacc0
[ipc2] receiver sys_wait ok=1 bits=0x2 (expect 0x2)
[sched] thread A iteration 2
[sched] thread B iteration 2
[ipc] client sys_call ok=1 reply_label=0x5eed (expect 0x5eed) reply_regs0=42 (expect 42)
[sched] thread A done
[sched] thread B done
```

## 수행한 작업

### 1. message 확장 (ipc.md §4)

[kernel/core/ipc/message.hpp](../../kernel/core/ipc/message.hpp) —
`page_descriptor`(vaddr/length/mode)와 `handle_transfer`
(src_handle/rights_mask)를 spec 그대로 추가했다. **스펙이 명시하지
않은 방향 규약을 정해 문서화했다**: 수신자는 `sys_recv`를 부르기
전에 `msg_out.pages[i]`에 "내가 받을 목적지"(vaddr/length)를 미리
채워 둬야 한다 — copy 모드는 "커널이 송신자 페이지 내용을 수신자가
미리 지정한 버퍼로 복사"하는 방식이기 때문이다(ADR-015). 이 규약이
없으면 목적지가 어디인지 커널이 알 방법이 없다.

### 2. kernel/core/ipc/endpoint.cpp — 전달 로직 (ipc.md §4, ADR-015)

`deliver_message()`(내부 헬퍼)를 추가해 `sys_call`/`sys_recv`의 두
핸드오프 지점(서버가 먼저 대기 중이던 경우, 호출자가 먼저 대기
중이던 경우) 모두에서 페이지·핸들을 실제로 옮긴다:
- **페이지(copy 모드만)**: 정렬 검사(페이지 정렬 아니면 호출자
  버그로 보고 `LIBK_PANIC`), 수신자 목적지 버퍼 크기 확인
  (`ipc_error::page_not_mapped`), `__builtin_memcpy`로 실제 바이트 복사.
  move/map은 계획대로 미구현 — 시도하면 `permission_denied`.
- **핸들**: M4의 `handle_table::create_proxy`를 그대로 재사용해
  `objects.md §4`의 위임 절차(권한 축소, 깊이 검사)를 수행하고,
  `handles[i].src_handle`을 새로 발급된 핸들 번호로 덮어쓴다(§4
  7단계, in/out 파라미터). 개별 핸들 위임 실패는 그 핸들만
  `INVALID_HANDLE`로 표시하고 나머지는 계속 진행한다(§4 3단계와
  같은 정신의 부분 실패).
- **알려진 단순화**: `sys_reply`는 스펙 시그니처 자체에 `handle_table`
  인자가 없어(ipc.md §3) 응답에는 페이지·핸들을 싣지 않는다
  (label+regs만, M6과 동일). 또한 송신자/수신자의 handle_table이
  지금은 같은 공유 테이블이다(M4/M6 데모와 동일한 "커널 컨텍스트
  하나" 단순화) — 실제 서로 다른 프로세스 간 위임은 M8 이후 실제
  프로세스가 생겨야 의미가 생긴다.

### 3. kernel/core/ipc/notification.cpp — sys_notify/sys_wait (ipc.md §7)

[kernel/core/ipc/notification.hpp](../../kernel/core/ipc/notification.hpp)/[.cpp](../../kernel/core/ipc/notification.cpp) —
`object::notification`(`atomic<uint64_t> bits` + 단일 waiter 포인터)을
`kernel_objects.hpp`에 추가했다. `sys_notify`는 CAS 루프로 비트를
OR하고(`atomic<T>`엔 `fetch_or`가 없어 ADR-072대로 필요한 조합만 CAS로
구현), 대기자가 있으면 `sched::enqueue()`로 깨운다. `sys_wait`는
현재 비트가 0이 아니면 원자적으로 읽고 clear해 즉시 반환하며, 0이면
`sched::block()`으로 블록한다. **스펙 표는 `sys_notify`를 `void`,
`sys_wait`를 맨 `u64`로 적어 "실패 없음"을 강조하지만, 핸들 자체가
무효/다른 종류일 수는 있다** — M6/M7의 다른 IPC 함수들과 일관되게
`result<>`로 감쌌다(핸들이 유효한 뒤부터는 스펙 그대로 절대 실패하지
않는다).

## 실행 중 발견해 고친 버그(중요 — 일반적인 수정으로 이어짐)

M6에서 "서버가 `sys_reply` 직후 곧장 hlt로 들어가 클라이언트를
방치"하는 버그를 한 번 겪고 그 자리에서만 `yield()` 한 번을 추가해
고쳤었다. M7에서 **같은 클래스의 버그가 다른 자리(송신자가 `sys_call`
반환 직후 곧장 hlt)에서 또 발생**했다 — QEMU 로그가 "sender sys_call
ok=1 ack_label=0xacc0"에서 완전히 멈추고(수신자의 `sys_wait` 로그도,
A/B의 마지막 반복도 전혀 안 나옴) 25초로 늘려도 재현되어 타이밍
문제가 아님을 확인했다.

이번엔 **개별 지점마다 땜질하는 대신 일반적인 해법으로 바꿨다**:
모든 데모 스레드가 마지막 무한 `hlt` 루프에 들어가기 직전에
`flush_yield()`(전체 스레드 수보다 넉넉한 16회 `yield()`)를 호출하도록
통일했다. `yield()`는 더 이상 돌릴 스레드가 없으면 그냥 즉시
반환하므로(비용은 회전할 스레드가 없을 때 무시할 만한 수준) 과하게
불러도 무해하다 — 이 한 번의 일반적 수정으로 M7의 새 버그뿐 아니라
**M5/M6에서 "정상"이라고 문서화했던 관찰 한계(스레드 B의 "done" 로그,
M6 클라이언트의 응답 로그가 안 보이던 것)까지 부수적으로 해소되어
이번 실행에서는 모든 로그 줄이 다 나온다.** M5/M6 완료 보고서는
그 시점의 관찰을 정직하게 기록한 것이므로 소급 수정하지 않았다 —
이 문서에 그 변화를 남긴다.

## 검증 결과 (정직하게 보고)

- **확인함**: 소스 페이지(알려진 패턴으로 채움)를 `sys_call`의
  `pages[0]`(copy 모드)로 보내고, 수신자가 미리 준비한 목적지 페이지에
  정확히 그 내용이 복사됨(`content_ok=1`, 1024개 uint32_t 전부 대조).
- **확인함**: 핸들 위임 — 송신자가 `handles[0]`으로 넘긴 notification
  소유 핸들이 수신자 쪽에서 **다른 핸들 번호**(원본과 동일한 객체를
  가리키는 새 프록시)로 도착함(`handle_info`로 `kind==notification`
  확인). 단순히 "핸들 번호가 왔다"만이 아니라, **그 새 핸들로 직접
  `sys_wait`를 걸어 실제로 동작하는 핸들임을 증명**했다 — `sys_notify`가
  (원본과는 또 다른 세 번째 프록시로) 보낸 비트(`0x2`)를 정확히
  받았다.
- **확인함**: `message_too_large`/`page_not_mapped`/`permission_denied`
  분기 자체는 코드로 존재하지만, 이번 데모는 전부 "정상 경로"만
  실행했다 — 실패 경로(목적지 버퍼 없이 보내기, move/map 시도,
  4개 초과 페이지 등)는 실행으로 검증하지 않았다. 코드 리뷰 수준
  (분기 로직 자체는 단순한 크기/모드 비교)으로만 확인했다.
- **확인하지 못함**: 서로 다른 handle_table(= 서로 다른 프로세스) 간의
  핸들 위임 — 지금은 송신자·수신자가 같은 공유 테이블을 쓴다(위
  "알려진 단순화" 참고). 진짜 프로세스 격리가 생기는 M8 이후 다시
  검증이 필요하다.
- **확인하지 못함**: `notification`에 여러 스레드가 동시에 `sys_wait`를
  거는 경우 — 지금 구조는 waiter를 하나만 추적한다(코드 리뷰로
  "이 범위에서는 충분하다"고 판단했을 뿐, 실제로 두 번째 waiter가
  들어왔을 때 첫 번째 waiter를 덮어써 버리는 것까지 실행으로
  확인하지는 않았다 — 실패 사례라기보다는 "지원 범위 밖"임을
  분명히 해 둔다).

## 다음 마일스톤과의 접점

- M8(initrun 로딩)이 `boot_info.initrd_addr`/`initrd_size`로 MCPACK을
  파싱해 ELF를 로드하고 유저모드로 진입시킨다 — 그 시점에 실제
  프로세스(별도 handle_table, 별도 address_space)가 처음 생기므로,
  이번 M7이 "공유 테이블"로 단순화했던 부분(페이지 vaddr 해석,
  핸들 위임 대상 테이블)을 실제 프로세스 경계에 맞게 다시 봐야 한다.
- move/map 전달 모드(ADR-015)는 여전히 미구현 — "이후 계획"으로
  남아 있다(kernel-bootstrap.md M7 범위 밖으로 이미 명시됨).
- devmgr 등 실제 서버가 착수되면(procsrv 이후) 이번에 완성한
  핸들·페이지 전달 경로가 그대로 재사용될 것이다 — kernel-bootstrap.md
  M7 계획이 명시한 목적("이후 devmgr 등 서버 착수 시 바로 쓸 수
  있도록") 그대로다.
