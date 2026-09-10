# 프로세스 서버(procsrv) 스펙

**관련 결정**: ADR-006, ADR-008, ADR-016, ADR-023, ADR-032, ADR-060~062,
ADR-074, ADR-075, ADR-079, ADR-080~088
**관련 설계**: [repo-layout.md](../design/repo-layout.md) (`servers/procsrv`),
[security-model.md](../design/security-model.md) (신뢰·권한·신원 전체 모델)
**관련 스펙**: [objects.md](objects.md) (핸들·`badge`·`confinement_tier`),
[ipc.md](ipc.md) (Call/Reply, `badge`), [memory.md](memory.md) §5(쿼터),
[vfs-layout.md](vfs-layout.md) (`/sys/proc`, `/usr/{사용자명}`),
[registry.md](registry.md) (`@global/system/*` 저장소, cfgsrv 프로토콜),
[boot.md](boot.md) §3(`cmdline` — 최초 부팅 부트스트랩)

이 문서는 지금까지 여러 ADR이 "procsrv 상세 설계 시점에 정한다"고
미뤄둔 사항들을 실제로 구현 가능한 수준으로 정의하기 시작한다. 범위가
넓어(§10 참고) 여러 라운드로 나눠 작성하며, 이번 라운드는 **모든
후속 설계(신원 배지, 신뢰·격리 등급 부여, fd 진실 공급원)가 딛고 설
"프로세스 테이블과 생성 시퀀스" 자체**를 다룬다.

## 1. 역할 요약

procsrv는 다음을 소유한다 (repo-layout.md, ADR-008):

- fork/exec/signal/wait/pid — POSIX 프로세스 의미론의 진실 공급원.
- fd 테이블의 진실 공급원(ADR-018) — 커널 핸들은 각 fd의 "로컬 별칭"일
  뿐, "이 fd가 실제로 무엇을 가리키는지"는 procsrv의 프로세스 테이블이
  기록한다.
- 신원(uid/gid/S·G·J, ADR-079)과 badge 발급(ADR-084)의 진실 공급원.
- `trusted`(ADR-074)·`confinement_tier`(ADR-085) 부여 권한의 위임 대상.
- `/sys/proc`, `/sys/live/sched` synthetic FS 서버(ADR-047, ADR-058).

## 2. `process_entry` — 프로세스 테이블

> **M27 실제 구현 각주**([security-model.md](../design/security-model.md)
> ADR-201): 이 §가 그리는 전체 구조(`fd_table`/`quota_state`/
> `identity_badge`/`children` 트리)는 여전히 목표 설계다. 실제로
> 구현된 것은 `pid`/`parent_pid`/`thread_handle`/`state`/`exit_code`
> 만 있는 부분집합이다 — 아래 §6 각주 참고.

```cpp
enum class process_state : uint8_t {
    running = 0,
    zombie  = 1,   // exit 했지만 부모가 아직 wait()하지 않음
};

struct fd_entry {
    uint32_t owner_kind;      // 이 fd를 실제로 처리하는 서버의 종류 식별자
                                // (예: fs, netsrv 등 — 정확한 enum 값은 각
                                // 서버 종류가 확정되는 시점에 정의, §10)
    handle   local_handle;    // 이 프로세스의 커널 핸들 테이블 인덱스
                                // (objects.md §1) — 실제 IPC는 이 핸들로 한다.
    uint64_t open_file_id;    // 소유 서버 쪽 "열린 파일 서술자"를 식별하는
                                // 값(서버가 발급, 의미는 서버마다 다름) —
                                // fork() 시 부모/자식이 같은 open_file_id를
                                // 공유해야 오프셋 공유(POSIX 의미론)가
                                // 성립한다(§4.1).
};

struct process_entry {
    uint32_t          pid;
    uint32_t          parent_pid;        // 0 = 부모 없음(initrun, 또는 로그인으로
                                           // 생성된 세션 프로세스, ADR-088 §결정 4)
    identity_badge    identity;          // uid/gid/S·G·J/jail_instance_id (ADR-084)
    handle            address_space;     // 커널 소유 핸들 (objects.md §2)
    handle            primary_thread;    // 커널 소유 핸들
    bool              trusted;           // address_space.trusted의 procsrv 측 사본 (ADR-063/074)
    confinement_tier  confinement;       // address_space.confinement_tier의 사본 (ADR-085)

    span<fd_entry>    fd_table;          // procsrv 슬랩 힙에 할당된 동적 배열
    quota_state       quota;             // memory.md §5 재사용

    process_entry*    parent;            // 부모 프로세스 테이블 항목 (nullptr = 없음)
    intrusive_list_hook siblings_hook;   // 부모의 children 리스트용 훅
    intrusive_list     children;         // 직계 자식들 (ADR-083의 guest 가시성 범위 계산에 사용)

    process_state     state;
    int32_t           exit_code;         // state == zombie일 때만 유효
};
```

- `trusted`/`confinement`를 커널의 `address_space`뿐 아니라 procsrv
  테이블에도 사본으로 둔 이유는, `/sys/proc`(§6) 조회나 badge 발급(§3)
  때마다 커널에 왕복 질의하지 않고 procsrv 스스로 즉시 판단하기
  위해서다 — 둘 다 프로세스 생성 시 딱 한 번 정해지고 이후 불변이므로
  (ADR-063/074/085) 사본이 원본과 어긋날 위험이 없다.
- `children`/`parent`는 커널 객체가 아니라 **procsrv 자신의 자료구조**다
  — 커널은 프로세스 간 부모-자식 관계를 전혀 모른다(ADR-002 HAL 경계와
  같은 맥락으로, "프로세스 계보"는 POSIX 의미론이므로 procsrv 소관).
  ADR-083의 guest 가시성 범위("자기 자신과 자손")는 이 트리를 그대로
  순회해 계산한다.
- **initrun 종료 시 재부모화**([boot-and-drivers.md](../design/boot-and-drivers.md)
  ADR-192, OPEN-51 해소) — initrun은 "커널 서버"(procsrv/vfs/devmgr
  등, 하드코딩된 이름으로 직접 spawn)를 전부 기동한 뒤, **유저
  서비스 관리자 데몬**(systemd류, 커널 서버가 아닌 유저 서비스만
  관리 — 이 데몬 자체는 procsrv.md 범위 밖의 별도 설계 대상)을
  마지막으로 하나 더 spawn하고서야 스스로 사라진다. 이 시점에
  procsrv는 initrun의 나머지 살아있는 자식(커널 서버들)의
  `parent_pid`/`parent`를 **그 유저 서비스 관리자 데몬**을 가리키도록
  갈아치운다 — 프로세스 트리의 영구 루트는 procsrv 자신이 아니라
  이 데몬이다. (이 데몬이 아직 구현되지 않은 동안에는 잠정적으로
  `parent_pid = 0`으로 둔다 — 재부모화 메커니즘 자체는 대상을
  매개변수로 받으므로, 데몬이 생기면 그 잠정 처리만 교체하면 된다.)

## 3. 프로세스 생성 — `fork()`

`fork()`는 **동일한 identity/trusted/confinement를 유지한 채** 새
`process_entry`를 만드는 경우다(신원이 바뀌는 것은 §4의 `exec()`에서
"실행 사용자 전환"이 일어날 때뿐).

1. 부모가 procsrv에 `op_fork` Call을 보낸다(§6).
2. procsrv는 `sys_recv`가 반환한 badge(ipc.md §3)를 부모의
   `identity_badge`로 확인한다(자기 자신의 프로세스 테이블에서 이미
   알고 있는 값과 대조 — 불일치는 있을 수 없다, badge는 재위임
   중에도 불변이므로).
3. procsrv가 커널에 새 프로세스(`address_space` COW 복제 + 새
   `primary_thread`)를 요청한다(ADR-016 결정 1, objects.md §2).
   커널은 `trusted`/`confinement_tier`를 **부모 값 그대로 복사**한다
   — 이 경로는 새로 등급을 "부여"하는 것이 아니라 기존 프로세스의
   복제이므로 ADR-074/085의 "부여 권한" 캐패빌리티가 다시 필요하지
   않다(부여는 최초 생성 시에만 의미 있는 연산).
4. procsrv가 새 `process_entry`를 할당해 자신의 프로세스 테이블에
   등록한다 — `identity`는 부모와 **동일한 값**(같은 uid/S·G·J,
   `jail_instance_id`도 동일 — 자식은 부모와 같은 jail 인스턴스에
   속한다), `parent`는 호출한 부모, 부모의 `children`에 연결한다.
5. procsrv가 새 프로세스에게 초기 시스템 서비스 badge 붙은 핸들
   (VFS, devmgr 등)을 발급한다(ADR-084 §4) — badge는 3단계에서 결정된
   `identity`를 그대로 인코딩하므로 부모 것과 동일한 badge가 된다.
6. **fd 테이블 복제**(§4.1의 프로토콜을 그대로 사용) — 부모의
   `fd_table`을 순회하며 각 `fd_entry.owner_kind` 서버에게 "이
   `open_file_id`를 자식에게도 열어달라"고 요청해, 부모·자식이 같은
   `open_file_id`를 공유하는 새 `fd_entry`를 자식의 `fd_table`에
   채운다.
7. procsrv가 `op_fork`에 대한 Reply로 부모에게는 자식의 `pid`를,
   자식에게는 `0`을 돌려준다(POSIX `fork()` 관례를 그대로 따름 —
   자식은 자신이 반환받은 IPC 결과가 아니라 실행 시작 시점에 이미
   `pid==0` 신호를 받아 "나는 자식이다"를 안다).

## 4. 프로세스 생성 — `exec()`

`exec()`는 두 가지가 섞여 있다: (a) 같은 프로세스의 실행 이미지 교체
(주소공간 내용을 새 ELF로 바꿈, `pid`는 유지), (b) 필요하면 **실행
사용자 전환**(예: 관리자가 특정 프로그램을 다른 uid로 실행하도록
설정한 경우 — 구체적 트리거 조건은 §10으로 미룸).

1. 호출자가 procsrv에 `op_exec` Call을 보낸다 — 실행할 경로(`pages[]`로
   전달, 긴 경로 대비)와 인자를 싣는다.
2. **guest/jail 실행 제한 검사(ADR-082)**: 호출자의 `effective(identity)`가
   guest 또는 jail이면, procsrv의 exec 로더가 대상 경로를 **심볼릭
   링크까지 모두 해석한 최종 경로**로 정규화한 뒤 `/run` 서브트리
   안인지 검사한다. 밖이면 즉시 `proc_error::exec_not_permitted`류로
   실패 반환(§6) — 이 시점 이후 단계는 전혀 진행하지 않는다.
3. **파일 읽기**: procsrv가 그 경로를 자신의 권한으로 열어 ELF를
   읽는다 — **호출자(특히 guest)의 VFS 권한이 아니라 procsrv 자신의
   권한으로 읽는다**. 이는 ADR-081의 "guest는 home 밖을 읽을 수
   없다"는 제약과 모순되지 않는다 — guest 본인이 파일을 읽는 것이
   아니라 procsrv가 대신 읽어 이미 검증된(2단계) `/run` 안의 파일을
   실행 이미지로 적재해 주는 것이기 때문이다(ADR-081 §영향에서
   "procsrv exec 설계 시점에 정한다"고 미뤄뒀던 답).
4. procsrv가 커널에 현재 `address_space`의 내용을 새 ELF로 교체하도록
   요청한다(페이지테이블 재구성, 스택/힙 재설정 — 커널 syscall의
   정확한 형태는 커널 프로세스 관리 syscall 설계 시점에 정한다).
   `pid`, `identity`, `trusted`, `confinement`, `parent`는 모두
   **그대로 유지**된다 — exec는 실행 이미지 교체이지 새 프로세스
   생성이 아니다.
5. `close-on-exec`로 표시된 fd(표시 방법은 §10)는 이 시점에 procsrv가
   `fd_table`에서 제거하고 해당 소유 서버에 닫기를 통지한다 — 나머지
   fd는 그대로 유지된다(POSIX 관례).
6. exec 성공 시 procsrv는 `op_exec`에 대해 응답하지 않는다 — 새 실행
   이미지가 이미 그 스레드에서 돌기 시작했으므로, 호출자 관점에서는
   `op_exec` 호출이 "성공하면 아예 반환하지 않고, 실패하면 오류가
   반환되는" 형태다(POSIX `exec()`와 동일한 관례).

### 4.1 fd 진실 공급원 프로토콜 (ADR-016/018 구체화)

procsrv는 각 `fd_entry.owner_kind`가 가리키는 서버에게 다음 오퍼레이션을
요구한다(각 서버는 자신이 노출하는 FS/네트워크 프로토콜에 이 오퍼레이션을
추가로 구현해야 한다 — ADR-018의 "FS 서버 프로토콜"에 대한 procsrv
전용 확장):

```cpp
// 각 소유 서버가 구현해야 하는 오퍼레이션(제안 label 값은 서버별
// IDL에서 정의 — 여기서는 의미만 고정한다).
// dup_for_new_client: 기존 open_file_id를 다른 프로세스에게도
//   유효하게 만들어, "같은 열린 파일 서술자를 공유하는" 새 핸들을
//   발급한다. 반환된 handle을 procsrv가 대상 프로세스에게 IPC
//   handle_transfer로 넘긴다.
result<handle, fs_dup_error> dup_for_new_client(uint64_t open_file_id, badge target_identity);
```

- **왜 커널의 일반 프록시(ADR-023)만으로는 부족한가**: 일반 프록시는
  "같은 커널 객체를 가리키는 rights 축소 뷰"만 만들 뿐, 파일의 읽기/
  쓰기 오프셋처럼 **서버가 자신의 상태로 들고 있는 값**(open file
  description)까지 부모·자식이 공유하게 만들지는 않는다 — POSIX
  `fork()`는 이 오프셋 공유가 의미론의 일부이므로, 커널 프록시가
  아니라 **소유 서버 자신이 참여하는 명시적 "복제" 연산**이 필요하다
  (ADR-016이 애초에 "각 fd가 속한 서버에 핸들 복제를 요청"이라고
  정해둔 이유가 바로 이것이다).
- **IPC 핸들 전달 개수 제한과의 관계**: `message.handle_count`는
  최대 `k_max_handle_transfers`(=2, ipc.md §4)다. `fd_table`이 이보다
  많으면 procsrv는 fd 여러 개를 **여러 번의 Call/Reply로 나눠**
  처리한다 — 한 번에 최대 2개씩, `fd_table.size()`가 다 소진될
  때까지 반복한다. 이는 성능보다 정확성을 우선하는 이 프로젝트의
  기본 방침(ADR-001)과 일치하며, fork()가 이미 상대적으로 무거운
  연산이므로 추가 왕복 비용은 감수한다.
- procsrv는 각 소유 서버로부터 받은 새 `handle`을 **자기 자신의
  핸들 테이블에 먼저 받은 뒤**, 그 핸들을 다시 자식 프로세스에게
  IPC handle_transfer로 넘긴다(부모→procsrv→자식의 2단 전달 — 자식은
  fork() 시점에 아직 존재하지 않아 소유 서버가 자식과 직접 통신할
  방법이 없으므로, procsrv가 유일하게 부모·자식 양쪽 모두와 이미
  통신 채널을 가진 중개자다).

## 5. 신뢰(`trusted`)·격리(`confinement_tier`) 등급의 최초 부여 — 개요

`fork()`/`exec()`는 항상 기존 프로세스의 신원을 그대로 물려받거나
유지한다(§3~4). 반면 **완전히 새로운 신원으로 프로세스를 시작**하는
경우 — 로그인(§8), 계정 생성 자체(§7) — 는 이미 있는 프로세스를
복제/치환하는 것이 아니라 처음부터 `identity`/`trusted`/`confinement_tier`를
결정해야 한다. [security-model.md](../design/security-model.md)
ADR-086이 이미 원칙을 정해뒀다: **신원 변경은 절대 기존 프로세스를
제자리에서 바꾸는 방식으로 하지 않고, 항상 새 `process_entry`를
만드는 방식으로만 한다.** §7~8이 이 원칙을 실제 절차로 구체화한다.

## 6. procsrv IPC 프로토콜 (오퍼레이션 개요)

> **M27 실제 구현 각주**([security-model.md](../design/security-model.md)
> ADR-201): 아래 `proc_op` enum은 여전히 목표 설계다(fork/exec을
> procsrv가 IPC로 "대행"한다는 모양이 실제 `sys_fork`/`sys_exec`
> syscall과 맞지 않는다는 것을 ADR-201이 인정한다). 실제로 구현된
> 것은 `mc/procsrv_protocol.h`(별도 헤더)의 `wait`(label=10)/
> `kill`(label=11)/`exit_report`(label=12)뿐이고, 이들은 procsrv
> 자신이 직접 스폰한 프로세스 사이에서만 동작하며 caller_pid는
> 자기주장 값이다(badge 검증 없음, OPEN-67). 정본 참조표는
> `docs/spec/generated/procsrv-wire.md`(ADR-195 자동 생성).

ipc.md §4의 `message.label`로 구분되는 procsrv 전용 오퍼레이션:

```cpp
enum class proc_op : uint32_t {
    fork             = 1,  // §3
    exec             = 2,  // §4 (regs: 없음, pages[]: 경로+인자 직렬화)
    wait             = 3,  // 자식 pid 대기, zombie 상태 회수(§2 process_state)
    getpid           = 4,
    signal           = 5,  // 세부는 §10
    fd_transfer      = 6,  // §4.1의 내부 왕복 전용 — 소유 서버 ↔ procsrv 간
    create_user      = 7,  // §7.1
    create_group     = 8,  // §7.1
    login_challenge  = 9,  // §8.2 — public_key 인증 1단계
    login            = 10, // §8.2 — 인증 완료 + 신규 세션 프로세스 생성
};

enum class proc_error : uint32_t {
    ok = 0,
    exec_not_permitted,      // ADR-082 — guest/jail의 /run 밖 실행 시도
    not_found,                // 대상 pid/경로/계정 없음
    invalid_state,            // 예: zombie가 아닌 자식을 wait()
    permission_denied,        // 예: super가 아닌 계정의 create_user 시도
    name_reserved,            // "global"을 계정/그룹 이름으로 시도 (ADR-060)
    already_exists,           // 이미 있는 계정/그룹 이름
    auth_failed,              // §8.3 — 계정 없음/자격 증명 불일치를 구분하지 않음
    challenge_expired,        // §8.2 — public_key 로그인 nonce TTL 만료
};
```

- 반환은 ADR-010 관례대로 `result<T, proc_error>`다.
- 정확한 `regs[]`/`pages[]` 배치(어떤 필드가 레지스터로 가고 어떤
  것이 페이지로 가는지)는 각 오퍼레이션을 실제로 구현하는 시점에
  IDL 스타일로 확정한다 — 이번 라운드는 오퍼레이션 목록과 의미만
  고정한다.

## 7. 계정 생성 (ADR-087)

### 7.1 `op_create_user`/`op_create_group`

```cpp
struct create_user_request {
    char        username[32];     // NUL 종료
    uint32_t    gid;               // 0xFFFFFFFF = "지정 안 함" → 전용 그룹 자동 생성
    bool        super_bit, guest_bit, jail_bit;
    auth_method auth;              // ADR-079 — password_hash 또는 public_key
    char        session_program[64]; // ADR-089 — 빈 문자열이면 "/run/bin/hello" 기본값 적용
};
// 반환: result<uint32_t /*새 uid*/, proc_error>

struct create_group_request {
    char     groupname[32];
    bool     super_bit, guest_bit, jail_bit;
};
// 반환: result<uint32_t /*새 gid*/, proc_error>
```

절차:

1. 호출자의 badge(`identity_badge`)에서 `effective.super`를 확인한다
   — `false`면 `proc_error::permission_denied`. (§7.2의 부트스트랩
   경로는 이 검사 자체를 건너뛴다 — IPC 호출이 아니므로 "호출자"가
   없다.)
2. `username`이 `"global"`이면(ADR-060 예약 스키마 이름)
   `proc_error::name_reserved`. `@global/system/users`에 이미 같은
   이름이 있으면 `proc_error::already_exists`.
3. `@global/system/counters`의 `next_uid`를 원자적으로 읽고 1
   증가시켜 새 `uid`를 얻는다(ADR-087 §결정 3 — procsrv 내부 락으로
   직렬화, 계정 생성은 저빈도 관리 작업이므로 성능보다 정확성 우선).
   대역 규칙(ADR-087 §결정 2: 0=root 전용, 1~999=시스템 서비스,
   1000+=사람)은 `create_user` 호출 시 명시적으로 시스템/사람 여부를
   구분하는 별도 인자로 받거나, 첫 사람 계정 생성 시점에 카운터를
   1000으로 미리 맞춰두는 방식 중 하나로 구현 시 정한다.
4. `gid`가 `0xFFFFFFFF`(미지정)면 같은 이름·같은 값(`uid==gid`)의
   전용 그룹을 `@global/system/groups`에 함께 생성한다(§7.1의
   `create_group` 절차를 내부적으로 재사용).
5. `auth.kind == password_hash`면 procsrv가 그 자리에서
   PBKDF2-HMAC-SHA256으로 해시해 저장한다(평문 비밀번호는 저장하지
   않는다) — `public_key`면 공개키를 그대로 저장한다.
6. `session_program`이 비어 있으면 `/run/bin/hello`를 채운다
   (ADR-089). `guest_bit`/`jail_bit`가 설정된 계정이면 이 값이
   `/run` 서브트리 안인지 지금 검증한다 — 밖이면
   `proc_error::exec_not_permitted`로 계정 생성 자체를 거부한다
   (로그인할 때마다 매번 실패하는 상황을 생성 시점에 방지, ADR-089
   §결정 4).
7. 완성된 `user_account`를 `@global/system/users`에
   `set_value(username, ...)`로 쓴다(cfgsrv의 registry.md §5 프로토콜,
   procsrv가 부팅 시 이미 보유한 전체 권한 `reg_table` 핸들 사용).
8. VFS에 `/usr/{username}/{bin,lib,etc,home}` 서브트리 생성을
   요청한다(vfs-layout.md §2) — 실패해도 계정 자체는 이미 생성된
   상태로 남는다(부분 실패 처리 방식은 구현 시 정한다).

### 7.2 최초 부팅 부트스트랩 (ADR-087 §결정 4)

procsrv가 시작할 때 `@global/system/users`가 비어 있으면:

1. `boot_info.cmdline`(boot.md §3)에서 초기 root 인증 정보를 읽는다
   (정확한 커맨드라인 문법은 §10에서 미결 사항으로 남김).
2. §7.1의 절차 2~7을 **호출자 검증(§7.1 1단계) 없이** 그대로 실행해
   `uid=0, super=true`인 root 계정을 만든다.
3. 이후 부팅에서는 `@global/system/users`가 더 이상 비어있지 않으므로
   이 경로가 다시 실행되지 않는다.

## 8. 로그인 (ADR-088)

### 8.1 로그인 프롬프트 프로세스

콘솔/세션마다 하나씩 떠 있는, 어떤 사람 계정 신원도 갖지 않는(uid는
ADR-087의 1~999 시스템 대역) 시스템 서비스다. `op_login_challenge`/
`op_login`을 호출하는 유일한 통상적 클라이언트다. 콘솔 드라이버와의
연결 방식은 §10에서 미결 사항으로 남긴다.

### 8.2 인증

```cpp
// password_hash 계정: 단일 Call.
struct login_request {
    char     username[32];
    uint8_t  password[64];   // ADR-079 — 최대 64바이트, 미사용 나머지는 0
    uint8_t  password_len;
};
// 반환: result<uint32_t /*새 세션 pid*/, proc_error>

// public_key 계정: 2단계.
struct login_challenge_request { char username[32]; };
// 반환: result<{uint64_t attempt_id, uint8_t nonce[32]}, proc_error>

struct login_response_request {
    uint64_t attempt_id;
    uint8_t  signature[64];  // nonce에 대한 Ed25519 서명
};
// 반환: result<uint32_t /*새 세션 pid*/, proc_error>
```

- `password_hash` 계정은 `op_login` 한 번으로 끝난다 — procsrv가
  저장된 salt·반복 횟수로 서버 측에서 해시를 계산해 상수 시간
  비교한다. 클라이언트에 salt를 절대 내주지 않는다.
- `public_key` 계정은 `op_login_challenge` → `op_login`(위 표에서는
  개념 구분을 위해 `login_response_request`로 표기했으나 실제
  `proc_op`는 §6의 `login` 하나를 재사용하고 페이로드로 구분한다)
  순서다. `attempt_id`별 nonce는 짧은 TTL(구체값은 구현 시 결정,
  수 초~수십 초 단위 권장)을 두고, 만료되면 `proc_error::challenge_expired`,
  검증에 성공하든 실패하든 해당 `attempt_id`는 **1회용으로 즉시
  폐기**한다(재전송 공격 방지).
- 계정이 없는 경우와 자격 증명이 틀린 경우 모두 동일하게
  `proc_error::auth_failed`를 반환한다(ADR-088 §결정 3 — 계정 존재
  여부를 외부에 노출하지 않는다).

### 8.3 신규 세션 프로세스 생성

인증 성공 시 procsrv는 로그인 프롬프트 프로세스를 그대로 둔 채(변형
없음, ADR-086) 다음을 수행한다 — [security-model.md](../design/security-model.md)
ADR-088 §결정 4의 절차를 그대로 구현한다:

1. `effective(user)`로 `identity_badge`를 구성한다. `effective.jail`이면
   새 `jail_instance_id`를 할당한다(재로그인마다 새 인스턴스 —
   이전 세션과 격리 공간을 공유하지 않는다).
2. 커널에 새 `address_space`+`primary_thread` 생성을 요청하면서,
   ADR-074/085로 위임받은 권한으로 `trusted=false`(사람 세션은
   신뢰 프로세스가 아님), `confinement_tier`(`effective.guest`→`guest`,
   `effective.jail`→`jail`, 아니면 `normal`)를 이 시점에 **처음
   설정**한다.
3. 새 `process_entry`를 프로세스 테이블에 등록한다 — `parent = nullptr`
   (로그인 프롬프트는 부모가 아니다, §5).
4. `confinement_tier == jail`이면 [filesystem.md](../design/filesystem.md)
   ADR-080의 에페메럴 오버레이 네임스페이스를 배정한다(홈 서브트리
   패스스루 포함).
5. ADR-084 §4와 동일하게 시스템 서비스 badge 붙은 초기 핸들들을
   발급한다.
6. `user_account.session_program`(기본값 `/run/bin/hello`, ADR-089)을
   `exec()`한다(§4의 일반 exec 절차 재사용 — guest/jail이면 ADR-082의
   `/run` 제한이 여기서도 그대로 적용되지만, §7.1 6단계에서 이미
   계정 생성 시점에 검증했으므로 로그인 시점에는 실패하지 않는다).
7. `op_login`(또는 `op_login`의 마지막 단계)에 대한 Reply로 새 세션의
   `pid`를 로그인 프롬프트에게 반환한다 — 로그인 프롬프트는 이후
   해당 세션을 소유하지 않으며, 자신의 콘솔에서 다음 로그인을 계속
   받는다.

## 9. M1~M8과의 관계

[kernel-bootstrap.md](../plan/kernel-bootstrap.md)는 M1~M8에서 procsrv
자체를 다루지 않는다(§범위 밖: "procsrv/vfs/memfs 등 실제 시스템
서버"). 이 스펙은 그 이후 계획(procsrv 착수 계획, 아직 작성 전)의
기반이 된다.

## 10. 아직 정하지 않은 것 (다음 라운드)

- **콘솔/세션 관리**: 로그인 프롬프트 프로세스와 콘솔/TTY 드라이버의
  연결 방식, 다중 콘솔 기동 방식은
  [boot-and-drivers.md](../design/boot-and-drivers.md) ADR-097과
  [security-model.md](../design/security-model.md) ADR-098로 해결됐다
  (OPEN-39 해소) — TTY는 `/sys/dev/tty{N}`로 노출되고, stdio는 §4.1의
  fd 복제 프로토콜로 로그인 프롬프트에서 새 세션으로 넘어간다. 콘솔
  드라이버 자체의 상세 프로토콜(문자 버퍼 형식 등)은 여전히 미정
  (OPEN-43, ADR-097 §영향).
- **권한 상승(ADR-075) IPC 프로토콜**: 요청 메시지 형식, 대화형 승인이
  필요한지, 캐패빌리티를 procsrv가 대행 발급하는지 각 서버가 직접
  발급하는지(ADR-075 §영향)는 여전히 미정이다. `process_entry`와
  §8의 로그인 절차가 이 프로토콜의 기반이 된다(요청자의 `identity`를
  procsrv가 이미 테이블에서 즉시 조회 가능).
- **쿼터(memory.md §5) 연동**: 새 프로세스의 초기 `quota_state` 값을
  누가·어떻게 정하는지(정책 서버 별도 존재 여부, ADR-024) 미정.
- **`identity_badge` 공통 헤더 위치, `jail_instance_id` 할당/회수
  정책**(ADR-084 §영향).
- **`/sys/proc`/`/sys/live/sched` FS 서버 프로토콜**(ADR-047/058,
  ADR-083의 가시성 필터링 포함).
- **`close-on-exec` 표시 방법** — fd 오픈 시점에 플래그를 어떻게
  받는지는 VFS `open()` 프로토콜 설계와 함께 정한다.
- **uid 시스템/사람 대역 전환 시점**(§7.1 3단계)의 정확한 구현 방식.
- **부트스트랩 커맨드라인 문법**(§7.2 1단계) — 평문 비밀번호를
  커맨드라인에 남기지 않는 방식(공개키 등록 또는 initrd 파일 참조)을
  우선 검토한다(ADR-087 §영향).
- **crypto 유틸리티**(PBKDF2-HMAC-SHA256, Ed25519, ADR-079)의 정확한
  소스 위치(`servers/procsrv/crypto/` 등).
- **로그아웃/세션 비정상 종료 시 정리 절차**(ADR-088 §영향).
- **su/sudo 전용 IPC 오퍼레이션**([security-model.md](../design/security-model.md)
  ADR-090/092/093/096) — §8의 로그인 절차를 재사용하되 부모=호출 셸,
  로그인 프롬프트 미경유, `exec()` 대상이 `session_program`이 아니라
  호출자가 넘긴 명령(경로+인자, 필수 필드), 그리고 인증 전에
  **`@global/system/delegates/<대상계정>` 위임 테이블을 먼저
  조회**(ADR-093)한다는 정책은 정해졌지만, 정확한 요청/응답 메시지
  형식(`op_delegate_grant`/`op_delegate_revoke`/영구 위임 확인용
  오퍼레이션 포함)은 아직 `proc_op`(§6)에 반영되지 않았다.
- **위임 만료 검사**(ADR-095/096) — `delegation_entry.granted_at`,
  `ttl_kind`(계정 기본값/명시적 기간/영구, ADR-096)와
  `@global/system/settings`/`@{계정명}/system/settings`의
  `delegation_timeout_seconds` 비교 단계를 위임 조회 절차에 추가해야
  한다. 영구 위임의 확인(수락) 절차(B가 대기 중인 위임을 조회·수락하는
  방법)도 함께 정의해야 한다.
- **위임 세부 범위**(ADR-093) — 명령 단위 제한은
  [security-model.md](../design/security-model.md) ADR-194로
  `delegation_entry`에 `allowed_command_paths`가 추가돼 해결됐다
  (§10 나머지 미결 항목과 함께 §6 `proc_op`에 반영해야 한다). 시간대
  단위 제한은 여전히 v1 범위 밖이다(기간/영구성은 ADR-096, 명령
  단위는 ADR-194로 해결) → **OPEN-42**(시간대 단위만 남음).
