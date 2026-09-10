# 설정 리포지터리(레지스트리) 스펙

**관련 결정**: ADR-006, ADR-011, ADR-060~064, ADR-074
**관련 설계**: [repo-layout.md](../design/repo-layout.md) (`servers/cfgsrv`)
**관련 스펙**: [ipc.md](ipc.md) (전송 계층), [objects.md](objects.md) (`trusted` 프로세스),
[vfs-layout.md](vfs-layout.md) (VFS와는 완전히 분리됨을 대조 확인)

Windows 레지스트리에서 착안한 시스템 설정 리포지터리다. **VFS와
완전히 분리된 전용 IPC 프로토콜**로만 접근하며(ADR-060), 파일 API로는
절대 도달할 수 없다.

## 1. 주소 체계

```
@스키마/A/B/table
```

- `@`는 스키마 이름의 프리픽스다.
- `@스키마`가 생략되면, 요청 프로세스를 실행한 **사용자의 스키마가
  강제**된다 — 즉 사용자 `alice`가 `A/B/table`이라고만 쓰면
  `@alice/A/B/table`로 해석된다.
- `global`은 **예약 스키마 이름**이며, 어떤 사용자도 `global`이라는
  사용자명을 가질 수 없다(procsrv의 사용자 계정 생성 규칙에 반영
  필요).
- `A`, `B`처럼 스키마와 leaf 테이블 사이의 세그먼트는 **중간 경로**
  (디렉토리처럼 계층을 구성)이고, 마지막 세그먼트가 실제 키-값을
  담는 **테이블**이다.
- 세그먼트 이름 규칙(허용 문자, 길이 제한)은 구현 시 정한다.

## 2. 데이터 모델 (ADR-061)

```cpp
enum class reg_value_type : uint8_t {
    string  = 0,
    int64   = 1,
    boolean = 2,
    binary  = 3,
};

struct reg_value {
    reg_value_type type;
    // 실제 페이로드는 IPC 메시지의 regs(소량) 또는 pages(대용량 binary/string)로 전달 — §5
};
```

테이블은 `이름(string) → reg_value` 사전이다. 여러 행(row)을 갖는
관계형 모델은 채택하지 않는다(ADR-061).

## 3. 권한 모델 (ADR-062)

중간 경로와 테이블 각각에 다음 메타데이터가 붙는다:

```cpp
struct reg_permissions {
    uint32_t owner_uid;
    uint32_t group_gid;

    // 각 3비트: R(읽기) W(쓰기) X(중간경로=목록조회 / 테이블=모든 CRUD)
    uint8_t owner_rwx;   // 소유자
    uint8_t group_rwx;   // 그룹
    uint8_t other_rwx;   // 기타 전체

    uint8_t special_bits; // 확장용 특수 비트(setuid류). 의미는 미정 — §7
};
```

- `X`는 중간 경로에서는 "하위 목록을 나열할 수 있는가", 테이블에서는
  "이 테이블에 대한 모든 CRUD(생성·조회·수정·삭제)를 수행할 수
  있는가"를 뜻한다(ADR-062).
- `owner_uid`/`group_gid`의 발급·관리 주체는 procsrv의 사용자 계정
  모델과 연동되어야 하며, 아직 procsrv가 상세 설계되지 않아 정확한
  절차는 이후 결정한다.

## 4. 커널 객체 종류

`cfgsrv`는 자체 프로세스이며(ADR-006 직접구현), 등록/오픈된 테이블
핸들은 ADR-011의 핸들 테이블 메커니즘을 그대로 쓴다 —
`object_kind`(objects.md §2)에 `reg_table`을 추가한다:

```cpp
enum class object_kind : uint32_t {
    thread        = 0,
    address_space = 1,
    endpoint      = 2,
    notification  = 3,
    reg_table     = 4,   // cfgsrv가 발급하는 열린 테이블 핸들
};
```

`reg_table` 핸들도 ADR-023의 프록시 메커니즘으로 위임 가능하다 —
예를 들어 cfgsrv가 어떤 프로세스에게 특정 테이블에 대한 읽기 전용
핸들만 위임하고 싶으면, `rights`를 read-only로 축소한 프록시를
발급하면 된다(objects.md §3~4의 일반 메커니즘 재사용).

## 5. 프로토콜

cfgsrv가 노출하는 엔드포인트에 보내는 메시지(ipc.md §4의 `message`
구조를 그대로 사용, `label`로 오퍼레이션 구분):

```cpp
enum class reg_op : uint32_t {
    open_table    = 1,  // regs: 경로 문자열은 pages[]로 전달 (긴 경로 대비)
    create_table  = 2,
    delete_table  = 3,
    list_children = 4,  // 중간 경로 나열
    get_value     = 5,  // handle(reg_table) + key
    set_value     = 6,  // handle(reg_table) + key + reg_value
    delete_value  = 7,
    list_values   = 8,  // 테이블 내 모든 key 나열
    set_permissions = 9, // reg_permissions 갱신 (소유자만)
};
```

- 경로 문자열, 문자열/바이너리 값처럼 레지스터 4개(`k_message_registers`,
  ipc.md §4)로 부족한 데이터는 `page_descriptor`(copy 모드)로 전달한다.
- 각 오퍼레이션은 §3의 권한 검사를 통과해야 한다 — 실패 시 ADR-010
  관례에 따라 `result<T, reg_error>`로 반환한다.

### 5.1 M19 구현의 실제 wire 매핑 (registry-decisions.md ADR-169)

`open_table`/`create_table`은 성공 시 **`message.handles[0]`가 아니라
`message.regs[1]`에 cfgsrv 자신이 관리하는 프로토콜-레벨 정수**를
반환한다 — ADR-169 §결정4가 정한 대로, 진짜 커널 `reg_table` 객체 +
런타임 객체 생성 syscall이 아직 없어(`servers/vfs`/`servers/fs/memfs`의
`open_file_id`와 같은 선례) 이 정수를 그대로 재사용한다. 이후
`get_value`/`set_value`/`delete_value`/`list_values`/`set_permissions`은
이 정수를 `message.regs[]`로 실어 보낸다.

경로 기반 오퍼레이션(`open_table`/`create_table`/`delete_table`/
`list_children`)의 요청: `regs[0]`=호출자 uid, `regs[1]`=호출자
사용자명(8바이트, NUL 패딩 — `@스키마` 생략 시 이 이름이 스키마가
된다), `pages[0]`=경로 문자열(`@스키마/A/B/table` 또는 `A/B/table`).
응답: `regs[0]`=`reg_error`, `regs[1]`=테이블 정수(성공 시에만 유효).

핸들 기반 오퍼레이션의 요청/응답:

| 오퍼레이션 | 요청 | 응답 |
|---|---|---|
| `get_value` | `regs[0]`=uid, `regs[1]`=테이블 정수, `pages[0]`=key | `regs[0]`=`reg_error`, `regs[1]`=`reg_value_type`, `regs[2]`=값 길이, `pages[0]`=값 바이트(int64=8바이트 LE, boolean=1바이트) |
| `set_value` | `regs[0]`=uid, `regs[1]`=테이블 정수, `regs[2]`=`reg_value_type`, `regs[3]`=값 길이, `pages[0]`=key, `pages[1]`=값 바이트 | `regs[0]`=`reg_error` |
| `delete_value` | `regs[0]`=uid, `regs[1]`=테이블 정수, `pages[0]`=key | `regs[0]`=`reg_error` |
| `list_values` | `regs[0]`=uid, `regs[1]`=테이블 정수 | `regs[0]`=`reg_error`, `regs[1]`=개수, `pages[0]`=NUL로 구분된 key 이름 목록 |
| `set_permissions` | `regs[0]`=uid, `regs[1]`=테이블 정수, `pages[0]`=`reg_permissions` 원시 바이트 | `regs[0]`=`reg_error` |

`list_children` 응답: `regs[0]`=`reg_error`, `regs[1]`=개수,
`pages[0]`=NUL로 구분된 하위 이름 목록.

권한 판정(ADR-169 §결정1 — group 미검증)은 호출자 uid가 테이블의
`owner_uid`와 같으면 `owner_rwx`, 아니면 `other_rwx`만 본다(`group_rwx`는
항상 대상에서 제외). uid 0(procsrv/root 관례, security-model.md
ADR-075의 "ROOT 전권"과 일관)은 모든 검사를 통과한다. `create_table`은
스키마가 호출자 자신의 사용자명과 같을 때(자기 스키마에 자유롭게
생성) 또는 uid 0일 때만 허용한다 — 그 외엔 `permission_denied`.
`list_children`은 이번 라운드엔 중간 경로에 별도 권한 메타데이터를
두지 않아(§3 "아직 정하지 않은 것"과 동일한 유예) 항상 허용한다.
신원(uid/사용자명)은 VFS의 guest/jail 신원(fs-protocol.md v3)과 같은
정신으로 **호출자가 스스로 선언**하며 커널이 강제하지 않는다(OPEN-38과
같은 제약).

```cpp
enum class reg_error : uint32_t {
    ok = 0,
    not_found,
    permission_denied,
    already_exists,
    invalid_path,       // 예: 잘못된 스키마 이름, 'global'을 사용자명으로 시도 등
    type_mismatch,       // get_value 시 요청 타입과 저장 타입 불일치
};
```

## 6. 비밀 데이터 보호

### 6.1 raw 값 반환 (ADR-064)

비밀/민감 항목도 다른 항목과 동일하게 다룬다 — §3 권한 검사를
통과하면 `get_value`는 항상 raw 값을 반환한다. "연산 위임"(서명·
복호화만 가능) 방식의 API는 두지 않는다. 보호는 순전히 (a) §3의
접근 통제와 (b) §6.2의 cfgsrv 프로세스 보호에 의존한다.

### 6.2 cfgsrv의 신뢰 프로세스 보호 (ADR-063)

cfgsrv의 `address_space` 객체(objects.md)는 `trusted` 플래그를 갖는다:

```cpp
struct address_space_trust_fields {
    bool trusted;   // true면 다른 어떤 프로세스도 이 주소공간을
                     // 디버그 접근(메모리 읽기, ptrace류 attach)할 수
                     // 없다 — 관리자 권한 프로세스도 예외 없음.
};
```

- 커널은 디버그/introspection을 요청하는 syscall(현재 프로젝트에는
  아직 존재하지 않음)에서 대상이 `trusted`이면 무조건 거부해야 한다
  — 이는 그런 syscall이 나중에 추가될 때 지켜야 할 선제적 제약이다.
- `trusted` 플래그를 설정할 수 있는 권한(누가, 언제 부여하는지)은
  [security-model.md](../design/security-model.md)의 ADR-074가 정한다 —
  커널→initrun(무조건) → initrun→cfgsrv(기동 시, 위임 캐패빌리티 사용)
  순서의 위임 체인이다.
- 스왑 서브시스템이 생기면 `trusted` 프로세스의 페이지는 스왑
  금지 대상이 되어야 한다(ADR-063 영향 항목, 아직 스왑 자체가 미설계).

## 7. 저장 백엔드 (구현 세부, 프로토콜 사용자에게는 비가시)

레지스트리 자체는 VFS로 노출되지 않지만, cfgsrv **내부적으로는**
자신의 데이터를 영속화할 저장소가 필요하다 — 이는 일반 VFS
경로(예: `/sys/etc` 밑의 전용 바이너리 파일)를 cfgsrv가 일반
프로세스로서 열어 쓰는 것으로 충분하며, 이 저장 방식은 레지스트리
프로토콜 사용자에게 전혀 노출되지 않는다.

### M19 구현의 실제 저장 위치/형식 (registry-decisions.md ADR-169)

경로는 `/sys/etc/registry.dat`다(VFS 마운트 테이블에 `/sys/` 전용
프리픽스가 없어 기본 라우팅으로 memfs의 평평한 이름공간에
`sys/etc/registry.dat`로 떨어진다 — VFS/memfs 변경 불필요). cfgsrv는
부팅 시 이 경로를 읽어 전체 테이블 상태를 복원하고, 쓰기 오퍼레이션
(`create_table`/`delete_table`/`set_value`/`delete_value`/
`set_permissions`)마다 전체 상태를 다시 직렬화해 덮어 쓴다. memfs가
seek/truncate를 지원하지 않으므로(fs-protocol.md v3), 직렬화 포맷
맨 앞에 4바이트 매직 + 4바이트 버전 + 8바이트 `payload_len`을 둬
로드 시 이전 라운드의 더 길었던 내용이 파일 뒤쪽에 남아 있어도
`payload_len`만큼만 유효하다고 취급한다.

## 8. `@global/system/services` — 유저 서비스 유닛 테이블 (registry-decisions.md ADR-196/197)

새 값 타입이나 새 프로토콜을 요구하지 않는 스키마 예시다 — §2/§5의
기존 모델(바이너리 타입 값)을 그대로 쓴다.

- **테이블**: `@global/system/services`(단 하나) — 키는 서비스
  이름(최대 32바이트), 값은 [boot-and-drivers.md](../design/boot-and-drivers.md)
  ADR-196의 `service_unit` 구조체를 그대로 담은 **바이너리 타입**
  값이다. cfgsrv 자신은 이 값의 내부 구조를 해석하지 않는다(§6.1
  "raw 값 반환"과 같은 정신) — 해석은 `servers/svcmgr`(소비자)의
  몫이다.
- **권한**(§3): owner = root(uid 0), owner만 RW, other는 R만 —
  일반 계정은 목록/내용을 조회할 수 있지만 등록·수정·삭제는
  root(또는 root의 위임을 받은 프로세스)만 가능하다.
- **소비자**: `servers/svcmgr`가 부팅 시 `list_values`+`get_value`로
  전체를 읽고, 런타임 등록/해제는 `set_value`/`delete_value`를
  그대로 쓴다(§5의 기존 `reg_op` 9종 중 4개만으로 충분하다) — 이
  용도로 registry.md 프로토콜 자체를 확장하는 부분은 없다.

## 아직 정하지 않은 것

- `owner_uid`/`group_gid`는 [security-model.md](../design/security-model.md)
  ADR-079의 `user_account`/`group_account`(각각 `@global/system/users`,
  `@global/system/groups` 테이블에 저장)가 발급 주체다 — 이 두 테이블
  자체를 registry.md의 §1~3 형식으로 구체적으로 스키마화하는 작업은
  아직 여기 반영되지 않았다(다음 registry.md 갱신 대상).
- `special_bits`(§3)의 정확한 의미 — setuid류 확장이 실제로
  필요해지는 시점에 정의한다.
- 변경 알림(레지스트리의 `RegNotifyChangeKeyValue`류) 지원 여부 —
  `/sys/live`(ADR-044/058)의 "즉시 반영" 요구사항과는 별개로,
  레지스트리 자체에 구독형 알림이 필요한지는 아직 논의되지 않았다.
- 트랜잭션(여러 `set_value`를 원자적으로 묶는 것) 지원 여부.
