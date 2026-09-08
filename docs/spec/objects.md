# 커널 객체·핸들 스펙

**관련 결정**: ADR-011, ADR-012, ADR-016, ADR-023, ADR-027, ADR-029, ADR-032, ADR-063
**관련 설계**: [repo-layout.md](../design/repo-layout.md) (`kernel/core/object`)
**관련 스펙**: [ipc.md](ipc.md) §4(핸들 전달), §8(체인 정책)

이 문서는 ADR-011이 정한 "프로세스당 단순 핸들 테이블" 모델과, ADR-023의
프록시 위임·철회 메커니즘을 구현 가능한 수준으로 구체화한다.

## 1. 핸들

핸들은 프로세스별 핸들 테이블의 인덱스(`u32`)다. `0`은
`INVALID_HANDLE`로 예약되어 있어 어떤 객체도 가리키지 않는다. 핸들은
프로세스 로컬이다 — 같은 숫자값이 다른 프로세스에서는 다른 객체(또는
아무 객체도 아닌 것)를 가리킬 수 있다.

## 2. 객체 종류

```cpp
enum class object_kind : uint32_t {
    thread        = 0,
    address_space = 1,
    endpoint      = 2,
    notification  = 3,
    reg_table     = 4,   // cfgsrv가 발급하는 레지스트리 테이블 핸들 (registry.md §4)
};
```

이 목록은 M1~M8(kernel-bootstrap.md) 범위를 넘어서는 것까지 포함한
현재까지의 전체 집합이다(`reg_table`은 registry.md 작성 시 추가됨).
이후 서버들이 필요로 하는 종류(예: 메모리 객체, MMIO 영역
캐패빌리티)가 더 추가될 수 있다.

### 2.1 address_space의 `trusted` 속성 (ADR-063)

`address_space` 객체는 `trusted` 불리언 필드를 갖는다. `true`이면
다른 어떤 프로세스도(관리자 권한 프로세스 포함) 이 주소공간에
디버그 접근(메모리 읽기, ptrace류 attach)을 할 수 없다 — 커널이
syscall 수준에서 무조건 거부한다. 이 프로젝트에는 아직 그런
디버그 syscall 자체가 없으므로, 이는 향후 추가될 기능이 지켜야 할
선제적 제약이다. `trusted`를 설정할 수 있는 권한·절차는 미정이다
(**OPEN-30**, registry.md §6.2에서 cfgsrv 보호 용도로 처음 요구됨).

## 3. 핸들 테이블 항목과 프록시 트리 (ADR-023, ADR-032)

각 커널 객체는 정확히 하나의 **소유(owner) 핸들**을 가지며, 그 위에
0개 이상의 **프록시**가 트리 형태로 위임될 수 있다. 소유 핸들을 갖지
않고 객체에 접근하는 방법은 없다 — 다른 프로세스는 항상 프록시를
통해서만 접근한다.

```cpp
struct handle_entry {
    object_kind kind;
    uint32_t    rights;          // 비트마스크. 의미는 kind에 따라 다름
                                   // (예: endpoint는 CAN_SEND/CAN_RECV/
                                   //  CAN_MOVE/CAN_MAP, ADR-029)
    void*       object;           // 커널 내부 포인터. 유저에게 절대 노출 안 함
    bool        valid;            // false면 철회된 상태 — 모든 연산이 실패

    // 트리 메타데이터 (소유 핸들은 parent == nullptr)
    handle_entry* parent;
    intrusive_list children;      // 이 노드에서 파생된 프록시들 (약한 참조,
                                   // 소유권 아님 — 오직 cascade revoke용)
    uint32_t    depth;             // 루트로부터의 위임 단계 수 (ADR-032)
};
```

- `rights`는 프록시 생성 시 부모의 부분집합만 가질 수 있다(ADR-029) —
  즉 `child.rights == (parent.rights & requested_mask)`이고
  `requested_mask`가 `parent.rights`를 벗어나는 비트를 요구하면 거부된다.
- `depth = parent.depth + 1`이며, 소유 핸들의 `depth = 0`이다.

## 4. 핸들 전달 (ipc.md §4의 `handles[]` 처리 절차)

`sys_call`/`sys_reply`가 `message.handles[0..handle_count)`를 처리하는
순서:

1. 각 `handle_transfer{src_handle, rights_mask}`에 대해 송신자의
   `src_handle`이 유효한지(`valid == true`) 확인한다. 무효하면 전체
   IPC 호출이 `ipc_error::invalid_handle`로 실패한다.
2. `new_rights = src_entry.rights & rights_mask`를 계산한다.
   `rights_mask`가 `src_entry.rights`에 없는 비트를 요구해도 오류는
   아니다 — 그냥 해당 비트가 빠진 채로 위임된다(요청자가 실수로 더
   요구해도 안전하게 축소됨).
3. `src_entry.depth + 1 > k_max_proxy_chain_depth`(기본 64, ADR-032)이면
   `ipc_error::permission_denied`로 그 핸들 전달만 실패 처리한다(IPC
   자체는 계속 진행 — 부분 실패는 수신자에게 `handle_count`가 실제보다
   적게 채워지는 방식으로 나타난다. 구체적인 부분 실패 보고 형식은
   구현 시 정한다).
4. 순환 검사: `src_entry`가 이미 수신자 프로세스 소유의 노드에서 파생된
   프록시라 하더라도(즉 왕복 위임) 새 프록시는 `src_entry`를 부모로
   하는 **새 노드**이므로 구조적으로 순환이 생기지 않는다(§6 참고) —
   따라서 이 단계에서는 깊이 검사만으로 충분하다.
5. 수신자의 핸들 테이블에 새 `handle_entry`(kind, `new_rights`, 같은
   `object` 포인터, `parent = src_entry`, `depth = src_entry.depth + 1`)를
   할당하고, `src_entry.children`에 이 노드를 등록한다.
6. 수신자 쪽 `message.handles[i].src_handle` 필드를 새로 할당된 핸들
   번호로 **덮어써서** 반환한다 — 즉 이 필드는 송신 시엔 "무엇을
   보낼지", 수신 후엔 "무엇을 받았는지"를 담는 in/out 파라미터다.

## 5. 시스템 콜

| 이름 | 인자 | 반환 | 설명 |
|---|---|---|---|
| `sys_handle_close` | `handle` | `result<void, handle_error>` | §6의 철회 절차 수행 |
| `sys_handle_info` | `handle` | `result<{object_kind, rights}, handle_error>` | 디버깅/검증용 조회 |

```cpp
enum class handle_error : uint32_t {
    ok = 0,
    invalid_handle,
    already_closed,
};
```

핸들 자체를 **생성**하는 전용 syscall은 없다 — 객체 생성은 각 객체
종류에 특화된 syscall(예: 스레드 생성, 엔드포인트 생성)이 소유 핸들을
암묵적으로 만들어준다. 프록시 생성은 §4처럼 IPC의 부수 효과로만
일어난다(직접적인 "핸들 복제" syscall은 두지 않는다 — 항상 상대방에게
IPC로 넘기는 형태로만 위임이 일어나므로 위임 대상과 무관하게 임의
복제하는 경로가 없다).

## 6. 철회(revoke) 절차

`sys_handle_close(h)`가 가리키는 노드를 `N`이라 하면:

1. `N.valid`를 `false`로 설정한다. 이 시점부터 `N`을 가리키는 어떤
   연산도 `handle_error::invalid_handle`을 반환한다.
2. `N.children`을 순회하며 각 자식 노드에 대해 이 절차를 **재귀
   적용**한다(자식의 자식까지 전부 무효화된다). 이는 ADR-023이
   프록시를 도입한 근본 이유(정밀한 접근 회수)를 지키기 위한
   필수 동작이다 — 그렇지 않으면 회수 직전에 미리 재위임해 접근을
   영구화할 수 있다.
3. `N`이 프록시였다면 `N`을 부모의 `children` 목록에서 제거하고
   `N`의 메모리를 회수한다(ADR-012 슬랩 할당자로 반환).
4. `N`이 **소유 핸들**(`parent == nullptr`)이었다면, 위 2단계로 모든
   프록시가 이미 무효화된 뒤이므로 **객체 자체**(`N.object`)도 함께
   파괴한다 — 참조 카운트 기반의 "마지막 참조가 없어질 때까지 유지"
   방식이 아니라, 소유자가 닫으면 즉시 전체 트리가 사라지는 방식이다.
5. 이미 닫힌 핸들에 다시 `sys_handle_close`를 호출하면
   `handle_error::already_closed`를 반환한다.

이 정책은 프록시 트리가 부모→자식 단방향으로만 자라고(§3), 위임은
항상 "새 자식을 만드는" 형태(§4)라는 것과 결합해 순환이 구조적으로
불가능함을 보장한다: 어떤 프록시도 자신보다 `depth`가 낮은(즉 더
루트에 가까운) 노드를 부모로 가리킬 수 없다.

## 7. 스레드·주소공간 객체 (개요, ADR-016 관련)

이 스펙은 핸들 메커니즘 자체에 집중하며, `thread`/`address_space`
객체 내부의 상세 필드(레지스터 상태, 페이지테이블 루트, COW 참조
카운트 등)는 [memory.md](memory.md)와 [scheduler.md](scheduler.md)에서
각각 다룬다.

## 아직 정하지 않은 것

- §4의 3단계에서 언급한 "부분 실패"(일부 핸들만 전달 실패)의 정확한
  보고 형식은 구현 시 정한다.
- 객체 종류가 늘어날 때(메모리 객체, MMIO 캐패빌리티 등) `rights`
  비트마스크의 의미를 종류별로 별도 문서화할 필요가 있다.
