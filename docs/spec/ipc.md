# IPC 스펙

**관련 결정**: ADR-004, ADR-011, ADR-013, ADR-014, ADR-015, ADR-023, ADR-028, ADR-029, ADR-084
**관련 설계**: [repo-layout.md](../design/repo-layout.md), [security-model.md](../design/security-model.md) (`badge`를 통한 신원 전파, ADR-084)

## 1. 개요

두 개의 독립적인 원시(primitive)를 제공한다 (ADR-004):

- **Call/Reply** — 동기 클라이언트-서버 RPC.
- **Notification** — 비동기 소실 없는 비트셋 신호.

모든 접근은 프로세스별 핸들 테이블을 통한다 (ADR-011).

## 2. 커널 객체

| 객체 | 설명 |
|---|---|
| `endpoint` | Call/Reply가 오가는 대상. rights: `CAN_SEND`, `CAN_RECV`, `CAN_MOVE`, `CAN_MAP` (ADR-029) |
| `notification` | 64비트 대기 중 비트셋 하나 |
| `handle` | 프로세스별 handle table의 인덱스(`u32`). `0`은 예약(`INVALID_HANDLE`) |

- 엔드포인트를 다른 프로세스에게 위임할 때는 프록시 핸들(ADR-023)을 새로
  발급한다. 프록시의 rights는 원본의 부분집합만 가능하다 — 원본에 없는
  `CAN_MOVE`/`CAN_MAP`을 프록시가 새로 얻을 수 없다 (ADR-029).

## 3. 시스템 콜

| 이름 | 인자 | 반환 | 설명 |
|---|---|---|---|
| `sys_call` | `handle`, `const message* msg_in`, `message* msg_out` | `result<void, ipc_error>` | 송신 + 응답 대기(블록) |
| `sys_recv` | `handle`, `message* msg_out` | `result<badge, ipc_error>` | 호출 수신 대기(블록) |
| `sys_reply` | `const message* msg_in` | `result<void, ipc_error>` | 가장 최근 `sys_recv`로 받은 호출에 응답(M13, ADR-151 — `handles[]` 위임까지 지원하도록 확장, §3.1 참고) |
| `sys_notify` | `handle`, `u64 bits` | `void` | 대상 비트셋에 OR, 실패 없음 |
| `sys_wait` | `handle` | `u64` | 비트셋이 0이 아니면 즉시 반환 후 원자적 clear, 0이면 블록 |

- `badge`는 호출자를 식별하는 값으로, 엔드포인트 프록시 발급 시 정해지는
  식별자다(서버가 "누가 호출했는지" 구분하는 용도, ADR-023의 프록시
  체계와 연동). 정확한 부여·상속 규칙(재위임 시 불변)은
  [objects.md](objects.md) §3, procsrv가 신원 정보를 직접 인코딩하는
  방식은 [security-model.md](../design/security-model.md) ADR-084를 참고.
- `sys_reply`는 대응하는 `sys_recv`가 없는 상태에서 호출되면 아무 동작도
  하지 않는다(오류 아님 — 단일 스레드가 반드시 recv→reply 순서로 쓴다는
  전제하의 단순화; 잘못된 사용은 상위 계층의 버그로 취급).

### 3.1. M13 갱신 — `sys_reply`의 `handles[]` 지원과 서로 다른 프로세스 간 전달 (ADR-151)

M8~M12는 IPC의 실제 상대가 항상 커널 스레드(주소공간 없음)이거나
같은 커널 컨텍스트였다 — `message` 구조체 자체(label/regs/page_count/
handle_count)를 상대 스레드의 `reply_dest`/`recv_dest` 포인터에 그대로
써도 문제가 없었던 이유는, 그 포인터가 늘 커널 가상주소(모든
주소공간이 공유하는 higher-half)였기 때문이다. M13부터 실제 유저
프로세스 두 개(예: procsrv↔vfs↔memfs)가 IPC로 직접 통신하므로, 이
가정이 깨진다 — 커널이 이제 "이 메시지가 어느 프로세스의 어느
주소공간에 있는가"를 알고 그 프로세스의 페이지테이블로 vaddr을
번역해야 한다(구현: `kernel/core/ipc/endpoint.cpp`의 `copy_to_user`/
`copy_from_user`, arch 훅 `arch_translate_user_page`).

이 번역은 **메시지 구조체 자체**(label/regs/page_count/handle_count,
그리고 `handles[]` 위임의 커널 쪽 처리)에만 적용된다. `pages[]`가
가리키는 실제 데이터 버퍼의 내용 복사는 **아직도 같은 주소공간
전제**로 남아 있다(§4 참고) — 서로 다른 프로세스 사이에서 `pages[]`로
큰 데이터를 옮기는 것은 이 갱신의 범위 밖이며, 필요해지는 시점에
별도로 다룬다.

이 갱신으로 `sys_reply`도 `handles[]`를 옮길 수 있게 됐다 — VFS가
`open()` 응답으로 FS 서버 엔드포인트에 대한 핸들을 위임하는 것
([filesystem.md](../design/filesystem.md) ADR-018)이 바로 이 방향
(서버→클라이언트, 즉 reply 방향)이라 이 확장이 필요했다. 그래서
`sys_reply`는 이제 호출자 자신의 `handle_table`도 받고(핸들 위임의
소스 테이블), `result<void, ipc_error>`를 반환한다(전달 실패 시에도
블록하지 않고 caller를 깨우는 동작은 그대로다).

## 4. message 구조

```cpp
static constexpr size_t k_message_registers    = 4;
static constexpr size_t k_max_page_descriptors = 4;

enum class transfer_mode : uint8_t {
    copy = 0,   // 기본값 (ADR-015) — 항상 허용
    move = 1,   // 대상 엔드포인트에 CAN_MOVE 필요 (ADR-029)
    map  = 2,   // 대상 엔드포인트에 CAN_MAP  필요 (ADR-029)
};

struct page_descriptor {
    uint64_t      vaddr;    // 송신자 프로세스 내 가상주소 (페이지 정렬)
    uint64_t      length;   // 바이트 수 (페이지 정렬)
    transfer_mode mode;
};

static constexpr size_t k_max_handle_transfers = 2;

struct handle_transfer {
    uint32_t src_handle;    // 송신자 핸들 테이블의 인덱스
    uint32_t rights_mask;   // 위임 시 적용할 rights 축소 마스크 (AND, ADR-029)
};

struct message {
    uint32_t         label;                              // IDL 오퍼레이션 식별자
    uint32_t         page_count;                          // 0..k_max_page_descriptors
    uint32_t         handle_count;                        // 0..k_max_handle_transfers (objects.md §4)
    uint64_t         regs[k_message_registers];            // 소량 데이터 (ADR-013 레지스터 계층)
    page_descriptor  pages[k_max_page_descriptors];        // 대용량 데이터 (ADR-013 페이지 계층)
    handle_transfer  handles[k_max_handle_transfers];      // 핸들 위임 (ADR-011, objects.md §4)
};
```

- `label` + `regs`만으로 표현되는 짧은 메시지가 표준 경로다 — 대부분의
  제어 요청은 페이지 없이 오간다.
- 가변 길이 IDL 메시지(ADR-013 3항)는 v1에서는 `label`로 스키마를 식별하고
  큰 페이로드를 `pages[]`에 얹는 관례로 시작한다. 전용 IDL 컴파일러(코드
  생성 도구)는 이 스펙의 범위 밖이다.
- `handles[]`를 통한 핸들 위임의 정확한 처리 절차(프록시 생성, rights
  축소 검증, 체인 깊이 검사)는 [objects.md](objects.md) §4에서 정의한다 —
  ADR-011이 예고했던 "IPC 중 핸들 전달 동작"의 구체화다.

## 5. 도네이션과 우선순위 (ADR-028)

`sys_call` 처리 순서:

1. 대상 엔드포인트에 `sys_recv`로 대기 중인 서버 스레드가 있으면, 그
   스레드를 즉시 실행 가능 상태로 만들고 **호출자의 우선순위(ADR-014의
   승격 결과 포함)를 그 스레드에 일시 상속**시킨다.
2. 대기 중인 서버가 없으면 호출자를 엔드포인트의 대기열에 넣고 블록한다.
3. 서버가 `sys_reply`를 호출하면 응답이 호출자에게 전달되고, 서버 스레드는
   **원래 우선순위로 복귀**한다.

상속된 우선순위도 ADR-014의 하드 상한(커널 밴드를 절대 넘지 못함)을 그대로
따른다 — 호출자가 커널 밴드일 수 없으므로 자동으로 만족된다.

## 6. 에러 코드

```cpp
enum class ipc_error : uint32_t {
    ok = 0,
    invalid_handle,       // 핸들이 유효하지 않음
    wrong_object_type,    // 핸들이 가리키는 객체가 요청한 연산과 맞지 않음
    permission_denied,    // rights 부족 (예: CAN_MOVE/CAN_MAP 없음, ADR-029)
    message_too_large,    // page_count 초과 등
    page_not_mapped,      // 송신자 vaddr이 매핑되어 있지 않음
    cancelled,            // 상대 프로세스/스레드 종료로 인한 취소
};
```

반환은 ADR-010의 `result<T, E>` 관례를 따른다 — 예: `sys_call`은
`result<void, ipc_error>`.

## 7. Notification 세부

- `sys_notify`는 절대 실패하지 않는다 — 비트를 OR할 뿐이며 큐잉·버퍼링이
  없다(ADR-004). 대상이 아직 `sys_wait` 중이 아니어도 비트는 유지된다.
- `sys_wait`는 현재 비트셋이 0이 아니면 즉시 그 값을 반환하며 원자적으로
  0으로 clear한다. 0이면 0이 아닌 값이 도착할 때까지 블록한다.

## 8. 프록시 핸들 체인 정책 (ADR-032)

엔드포인트 프록시(§2, ADR-023)는 다른 프록시를 대상으로 다시 위임할 수
있어 체인을 이룰 수 있다. ADR-032에 따라:

- 순환 참조는 항상 금지된다 — 커널은 새 프록시 생성 시점에 체인을
  검사해 순환이 되는 요청을 거부한다.
- 체인 깊이는 기본값 64로 제한하며, 이 상한(`k_max_proxy_chain_depth`)은
  빌드 타임 기본값을 갖되 부팅 파라미터로 조정 가능해야 한다.

## 아직 정하지 않은 것

- 서버가 다중 클라이언트를 동시에 처리할 때(예: 여러 `sys_recv` 스레드)의
  대기열 정렬 정책(FIFO vs 우선순위 정렬)은 스케줄러 구현 시 확정한다.
