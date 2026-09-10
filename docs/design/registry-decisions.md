# 설계 결정: 설정 리포지터리(레지스트리)

cfgsrv 서브시스템(스키마·테이블 주소 체계, 권한 모델, 비밀 데이터 보호)에 관한 결정. [spec/registry.md](../spec/registry.md)의 근거가 되는 결정들이다.

[← 설계 문서 색인](index.md)

---

## ADR-060. 설정 리포지터리(레지스트리) 도입: 스키마·계층적 테이블·전용 프로토콜

- **상태**: 확정 (2026-09-08)
- **결정**: Windows 레지스트리류의 시스템 설정 리포지터리를 신설한다.
  - 주소 형식: `@스키마/A/B/table` — `@`가 스키마 이름의 프리픽스다.
  - `@스키마`가 생략되면 요청 프로세스를 실행한 **사용자의 스키마가
    강제**된다(스키마 이름 = 사용자명).
  - `global`은 **예약 스키마 이름**이며 사용자명으로 사용할 수 없다.
  - 테이블 이름 자체가 계층 구조를 가진다 — `A`, `B`는 스키마 내
    중간 경로(디렉토리처럼 동작), `table`이 실제 데이터를 담는 leaf다.
  - 접근은 **VFS와 완전히 분리된 전용 IPC 프로토콜**로만 이뤄진다 —
    파일 API(open/read/write)로는 절대 접근할 수 없다.
- **근거**: 타입 있는 값·세밀한 CRUD 의미론·향후 변경 알림 같은
  레지스트리 특유의 기능은 VFS의 바이트스트림 파일 의미론과 잘
  맞지 않는다. 사용자가 "파일시스템과 별도의 방식으로 CRUD 해야만
  한다"고 명시적으로 요구했다.
- **영향**:
  - 새 전용 서버 `servers/cfgsrv`가 필요하다(ADR-006 직접구현 대상).
    `repo-layout.md`의 `servers/` 목록에 추가해야 한다.
  - 기존 `/sys/etc`(ADR-044), `/sys/live`(ADR-047)와는 **완전히
    독립적인 별개 서브시스템**이다 — 서로의 백엔드가 되지 않는다.
  - 구체적 프로토콜은 [registry.md](../spec/registry.md)에서 정의한다.

## ADR-061. 레지스트리 값 모델: 타입 있는 키-값 사전

- **상태**: 확정 (2026-09-08)
- **결정**: 각 테이블은 이름→값의 키-값 사전이며, 값은 타입 태그를
  갖는다(문자열, 정수, 바이너리, 불리언 등 최소 집합에서 시작).
  여러 행(row)을 갖는 관계형 테이블 모델은 채택하지 않는다.
- **근거**: Windows 레지스트리에 가장 가까운 모델을 원한다는 사용자의
  명시적 선택. 설정값·보안 키 저장이라는 주 용도에 비해 관계형
  다중 행 스키마는 불필요하게 무겁다.
- **영향**: 값 타입 열거형과 직렬화 형식을 `registry.md`에서 정의한다.

## ADR-062. 레지스트리 권한 모델: Unix 스타일 RWX(소유자/그룹/기타) + 특수 비트

- **상태**: 확정 (2026-09-08)
- **결정**: 테이블과 중간 경로(스키마 내 디렉토리 역할) 각각에
  **소유자(owner) RWX, 그룹(group) RWX, 기타(other) RWX** 권한을
  둔다. `R`=읽기, `W`=쓰기, `X`=(중간 경로일 때) 하위 목록 조회 /
  (테이블일 때) 해당 테이블에 대한 모든 CRUD 허용. 여기에 더해
  특수 비트(setuid류 확장)를 위한 여지를 남긴다.
- **근거**: 사용자가 이 구조(그룹/소유자/기타 RWX + 특수 비트)를
  명시적으로 지정했다. 검증된 전통적 Unix 권한 모델의 단순성을
  재사용한다.
- **영향**:
  - cfgsrv는 각 경로/테이블 노드에 `owner_uid`, `group_gid`,
    권한 비트(rwxrwxrwx + 특수 비트)를 메타데이터로 저장해야 한다.
  - uid/gid 발급 주체(사용자·그룹 계정 관리)는 procsrv의 사용자
    계정 모델과 연동되어야 하며, procsrv가 아직 상세 설계되지
    않아 정확한 발급 절차는 이후 결정 대상이다.

## ADR-063. cfgsrv 프로세스 보호: 커널이 보장하는 introspection 차단 (신뢰 프로세스)

- **상태**: 확정 (2026-09-08)
- **결정**: cfgsrv는 여전히 평범한 유저랜드 프로세스(ADR-006 경계
  준수)이지만, 커널이 특별히 보호하는 **"신뢰 프로세스(trusted
  process)"** 속성을 가질 수 있다. 이 속성이 설정된 프로세스의
  주소공간은 **다른 어떤 프로세스도(관리자 권한 프로세스 포함)
  디버그 접근(메모리 읽기, ptrace류 attach)할 수 없다** — 커널이
  syscall 수준에서 무조건 거부한다.
- **근거**: 보안 키·비밀 데이터를 다루는 프로세스가 유저랜드에 있는
  이상, 다른 프로세스가(설령 관리자 권한이라도) 그 메모리를 들여다볼
  수 있다면 비밀이 실질적으로 보호되지 않는다. 커널이 이 규칙 하나만
  강제하면 마이크로커널 최소화 원칙(ADR-001/006/007)을 어기지 않고도
  실질적 보호를 얻을 수 있다 — 비밀 자체를 커널 안에 두는 것보다
  훨씬 작은 변경이다.
- **영향**:
  - 커널 객체 모델([objects.md](../spec/objects.md))의 `address_space`
    객체에 `trusted` 플래그가 추가되어야 한다.
  - 이 플래그를 설정할 수 있는 권한은 극히 제한적이어야 한다 —
    구체적 발급 절차(예: initrun이 기동 매니페스트에서 cfgsrv를
    시작할 때만 부여, 이후 변경 불가)는 미결정. → **미결정 (OPEN-30)**
  - 이 프로젝트에는 아직 디버그/ptrace/코어덤프 syscall 자체가
    존재하지 않는다(스펙 없음) — 이 ADR은 "그런 기능이 나중에
    추가되더라도 신뢰 프로세스는 예외 없이 차단된다"는 선제적
    제약으로 기록한다.
  - 스왑(swap) 서브시스템이 아직 설계되지 않았다 — 스왑이 생기면
    신뢰 프로세스의 페이지는 스왑 금지 대상이 되어야 한다는 제약을
    미리 기록해둔다.

## ADR-064. 레지스트리 비밀 항목 접근: 권한이 있으면 raw 값 반환

- **상태**: 확정 (2026-09-08)
- **결정**: 비밀/민감 항목(암호화 키 등)도 다른 테이블 항목과 동일하게
  취급한다 — 요청자가 ADR-062의 권한 검사를 통과하면 **raw 값을
  그대로 반환**한다. "연산 위임"(서명·복호화만 가능, 값 자체는 절대
  못 봄) 방식의 HSM/KMS류 API는 채택하지 않는다.
- **근거**: 사용자가 명시적으로 raw 조회 방식을 선택했다. 보호는
  접근 통제(ADR-062)와 cfgsrv 프로세스 자체의 커널 수준 보호
  (ADR-063)로 충분하다고 판단한다 — 연산 위임 모델은 API 복잡도만
  높이고 현재 범위에서 필수는 아니다.
- **영향**: 향후 필요해지면 개별 항목에 "연산 전용" 플래그를 추가하는
  확장은 배제하지 않는다 — 이 ADR은 v1 기본값이다.

## ADR-197. 유저 서비스 유닛 레지스트리 스키마 — `@global/system/services` 테이블, 새 값 타입/프로토콜 없이 기존 모델 재사용

- **상태**: 확정 (2026-09-10, 계획 단계 — [user-service-manager.md](../plan/user-service-manager.md)
  M41 착수 전에 전략만 먼저 결정한다. [boot-and-drivers.md](boot-and-drivers.md)
  ADR-196의 `service_unit` 구조체를 실제로 저장하는 자리를 정한다)
- **결정**: 새 전용 테이블 종류나 새 값 타입을 만들지 않는다 —
  **기존 레지스트리 모델(ADR-060/061)을 그대로 재사용**한다.
  1. 테이블 하나 `@global/system/services`를 만든다. 그 안에 서비스
     이름을 키로, ADR-196의 `service_unit` 구조체를 **바이너리
     타입 값**(ADR-061이 이미 지원하는 값 타입)으로 그대로 저장한다.
  2. svcmgr는 부팅 시 `list_values`(기존 `reg_op` 9종 중 하나,
     ADR-169)로 이 테이블의 모든 키를 나열하고, 각각 `get_value`로
     읽어 `service_unit`으로 그대로 캐스팅한다. 런타임 등록/해제
     (ADR-196 §결정7의 `op_register`/`op_unregister`)는 각각
     `set_value`/`delete_value`를 그대로 호출한다 — **cfgsrv 자신의
     프로토콜(registry.md)은 이 용도로 단 한 줄도 확장되지 않는다.**
  3. **권한**(ADR-062): 이 테이블의 owner는 root(uid 0)다. owner만
     RW, other는 R만(일반 계정은 목록/상태를 볼 수 있지만 등록·수정은
     못 함) — `create_user`(procsrv.md §7.1)가 이미 쓰는 "super만
     쓰기" 검사와 같은 정신이다. 이 권한 검사는 **cfgsrv 자신이**
     기존 ADR-062 권한 모델로 수행한다 — svcmgr가 별도로 권한을
     검사할 필요가 없다(오히려 검사하면 cfgsrv와 판정이 어긋날
     위험이 생긴다).
- **근거**: 레지스트리가 이미 "타입 있는 키-값" 모델(ADR-061)이고
  관계형 다중 행이 필요한 것도 아니다 — 서비스 목록은 정확히
  "이름→하나의 구조화된 값"이라는 KV 그 자체다. M19(ADR-169)가
  이미 `reg_op` 9종을 전부 구현해 뒀으므로, svcmgr 입장에서는 이미
  존재하는 클라이언트 코드(`libmc`의 cfgsrv 프로토콜 클라이언트,
  ADR-183 §결정4와 같은 "항상 `libmc` 경유" 원칙)를 그대로 호출하는
  것만으로 끝난다 — 새 서브시스템을 만들 이유가 없다.
- **영향**:
  - [registry.md](../spec/registry.md)의 "아직 정하지 않은 것"에서
    이 스키마를 실제 스키마로 옮겨 기록한다.
  - `service_unit`(ADR-196)의 정본은 여전히 `libmc/include/mc/
    svcmgr_protocol.h`다 — 레지스트리는 그 구조체를 불투명한
    바이너리 blob으로만 다룬다(cfgsrv 자신은 그 내용을 해석하지
    않는다, ADR-064와 같은 "raw 값 반환" 정신).

## ADR-169. M19 범위 좁힘: cfgsrv 최초 구현 — group 검증 제외 + 실제 VFS 영속화 + 프로토콜 9종 전부 + 프로토콜-레벨 정수 핸들

- **상태**: 확정 (2026-09-09)
- **결정**: `system-servers-bringup.md` §M19를 구현하며, 사용자에게
  `AskUserQuestion`으로 확인한 세 가지 범위 결정과, 구현 중 발견한
  네 번째 기술적 단순화를 기록한다.
  1. **group 권한 비트는 이번 라운드에서 검증하지 않는다.** procsrv의
     `account` 구조체는 M18에서 `uid`까지만 얻었고 `gid` 개념이 아예
     없다 — `registry.md` 자신도 `group_account` 테이블 스키마를
     "아직 정하지 않은 것"으로 남겨 뒀다. `reg_permissions.group_gid`/
     `group_rwx` 필드는 스펙 그대로 구조체에 유지하지만, 이번 라운드는
     owner(소유자)/other(기타) 두 경우만 실제로 행사한다 — 모든 테이블의
     `group_gid`는 0, `group_rwx`는 항상 0으로 두고 절대 매치시키지
     않는다.
  2. **cfgsrv는 실제로 VFS 파일에 영속화한다.** `registry.md` §7이
     이미 정한 대로 cfgsrv가 일반 프로세스로서 VFS 경로
     (`/sys/etc/registry.dat`, 기본 라우팅으로 memfs에 떨어진다)를 열어
     자신의 전체 테이블 상태를 저장/복원한다 — 부팅 시 로드, 쓰기
     오퍼레이션(`create_table`/`delete_table`/`set_value`/
     `delete_value`/`set_permissions`)마다 전체를 다시 직렬화해 저장한다.
     memfs가 seek/truncate를 지원하지 않으므로(M18 fs-protocol v3),
     저장 형식 맨 앞에 `payload_len`을 둬 로드 시 이전 라운드의 더 긴
     내용이 남아 있어도 정확히 그만큼만 읽는다.
  3. **`reg_op` 9종(open/create/delete_table, list_children, get/set/
     delete_value, list_values, set_permissions)을 전부 구현한다** —
     검증 목표 자체(왕복 확인)엔 `open_table`+`get_value`+`set_value`만
     있으면 충분하지만, 나머지도 이번에 갖춰 이후 마일스톤에서 다시
     손댈 필요를 없앤다.
  4. **`open_table`이 반환하는 "핸들"은 진짜 커널 `object_kind::reg_table`
     객체가 아니라 cfgsrv 자신이 관리하는 프로토콜-레벨 정수다** —
     `registry.md` §4/§5는 `message.handles[0]`에 진짜 커널 핸들을
     실어 반환한다고 적었지만, 실제로 확인해 보니 현재 커널의
     `handle_table::create_owner()`는 `sys_process_spawn`의 스폰
     시점(`create_endpoint`)에만 호출되고, 살아있는 유저 프로세스가
     런타임에 새 커널 객체를 직접 만드는 syscall이 아직 없다 —
     `servers/vfs`/`servers/fs/memfs`의 `open_file_id`도 실은 이 캡을
     피해 단순 정수로 설계된 선례다. 이번 라운드는 그 선례를 그대로
     재사용한다(`get_value`/`set_value`/`delete_value`/`list_values`/
     `set_permissions`은 이 정수를 `message.regs[]`로 받는다) — 진짜
     커널 `reg_table` 객체 + 런타임 객체 생성 syscall은 이후로 미룬다.
- **근거**: 결정 1~3은 사용자가 `AskUserQuestion`으로 직접 고른
  범위다(1은 권장 단순화를 선택, 2·3은 더 충실한 쪽을 선택 — M18의
  ELF 로더 선택과 같은 패턴). 결정 4는 사용자가 "진짜 커널 핸들로
  구현" 대신 명시적으로 "프로토콜-레벨 정수 핸들로 단순화(권장)"를
  선택했다 — VFS/memfs가 이미 같은 이유로 같은 선택을 한 선례가 있어
  일관성도 있다.
- **영향**:
  - [registry.md](../spec/registry.md)를 이 네 결정에 맞춰 갱신한다
    (§5에 구체적 wire 매핑 추가, §7에 실제 경로 명시, `open_table`
    반환값이 정수임을 명시).
  - `owner_uid`/`group_gid`가 실제로 필요한 group 검증, 진짜 커널
    `reg_table` 객체·런타임 객체 생성 syscall은 이후 마일스톤(또는
    필요해지는 시점)으로 미룬다 — [open-items.md](open-items.md)에
    새 OPEN 항목을 추가하지 않는다(둘 다 registry.md/procsrv.md가
    이미 "아직 정하지 않은 것"으로 표시해 둔 항목의 연장이라 새
    항목이 아니다).

## ADR-215. M41 완성: svcmgr가 `@global/system/services`를 실제로 읽음 — cfgsrv 값 저장 한도·영속화 버퍼가 예상보다 훨씬 작았음을 발견

- **상태**: 확정 (2026-09-10, [user-service-manager.md](../plan/user-service-manager.md)
  M41 실행 중 확정)
- **배경**: ADR-197(계획 단계)의 결정을 실제로 구현한다. 계획이
  예정한 것(svcmgr가 하드코딩 목록 대신 `list_values`+`get_value`
  로 테이블을 읽음, `depends_on` 위상정렬)은 전부 완성했지만, 실행
  중 cfgsrv의 기존 저장 한도가 `service_unit` 하나도 못 담을 만큼
  작다는 것과, 그걸 고치는 과정에서 훨씬 더 심각한 기존 메모리
  손상 버그를 발견했다.
- **결정**:
  1. `libs/mc/include/mc/cfgsrv_client.h`+`src/ipc/cfgsrv_client.c`
     (신규) — `open_table`/`create_table`/`get_value`/`set_value`
     (binary 타입)/`delete_value`/`list_values`의 얇은 클라이언트.
     procsrv 자신의 self-test가 M19부터 이 프로토콜을 인라인으로만
     썼던 것을 처음으로 재사용 가능한 `libmc` 모듈로 뽑았다(ADR-197
     이 예정해 둔 "이미 존재하는 클라이언트 코드" 자리를 실제로
     채운다).
  2. `libs/mc/include/mc/svcmgr_protocol.h`(신규) — ADR-196 §결정2의
     `service_unit` 구조체를 그대로 옮겼다(`mc_svcmgr_service_unit`).
  3. svcmgr는 부팅 시 `@global/system/services`를 열고(없으면
     만들고), 비어 있으면 자기테스트 유닛 둘(`svc-b`가 `svc-a`에
     `depends_on`)을 등록한다(M42의 `op_register`가 아직 없어서) —
     이후 `list_values`+`get_value`로 실제 테이블을 읽어
     `depends_on`을 단순 위상정렬해 순서대로 spawn하고, 각각
     ADR-193 준비완료 신호를 기다린다.
  4. `delete_value`로 `svc-b`를 지우고 다시 `list_values`를 불러
     실제로 목록에서 빠졌는지 확인한다 — 계획의 "지우고 재부팅해
     확인" 중 "재부팅" 부분은 범위를 좁혔다(cfgsrv의 저장 파일이
     기본적으로 memfs에 떨어져 진짜 재부팅을 거치면 사라진다,
     registry-decisions.md ADR-169 §결정2 — 디스크 기반 경로로
     바꾸는 것은 이 ADR의 범위 밖인 별개 결정이다). 같은 부팅 안에서
     "등록→소비→삭제→재조회"로 메커니즘 자체만 증명한다.
  5. `exec_path`(VFS 경로) 필드는 저장/조회는 되지만 아직 읽지
     않는다 — 등록된 유닛이 몇 개든 전부 M40과 같은 임베딩된 데모
     ELF를 실행한다. 서로 다른 실행 이미지를 VFS에서 읽어 오려면
     `mc/fs_client.h`에 쓰기 클라이언트(그 이미지들을 VFS에 미리
     심을 방법)까지 새로 필요해 이번 라운드 범위를 벗어난다고
     판단했다.
- **실행 중 발견 1 — `mc/cfgsrv_client.c`의 페이지 버퍼에 정렬이
  없어 커널이 즉시 패닉했다**: `kernel/core/ipc/endpoint.cpp`가
  `page_descriptor.vaddr`을 4096바이트 경계로 강제하는데(M13/
  ADR-151), 새로 만든 클라이언트의 정적 버퍼 3개(경로/키/값)에
  `alignas`를 빠뜨려 "page_descriptor not page-aligned"로 즉시
  패닉했다 — `servers/procsrv/main.cpp`의 같은 용도 버퍼들이 이미
  `alignas(k_page_size)`를 쓰고 있던 이유가 정확히 이거였다. 셋
  다 `_Alignas(MC_CFG_PAGE_SIZE)`를 추가해 고쳤다.
- **실행 중 발견 2 — cfgsrv의 값 저장 한도(256바이트)가
  `service_unit`(~616바이트)보다 훨씬 작았다**: `k_max_value_len`
  이 M19 시점 문자열 값(계정 설정 정도)만 염두에 두고 정해진
  256바이트였다 — 실제 길이가 이를 넘으면 조용히 잘려 저장되고,
  다시 읽었을 때 길이가 기대와 안 맞아 svcmgr의 `load_units`가
  매번 실패했다. 1024로 올렸다.
- **실행 중 발견 3 — cfgsrv의 영속화 버퍼가 그 한도 상승 후
  진짜로 넘칠 수 있었고, 실제로 다른 정적 변수를 덮어써
  `[shell] cat` 자기테스트까지 깨뜨렸다**: `persist_save()`가
  전체 테이블 상태를 8KiB 정적 버퍼(`g_persist_buf`)에 경계 검사
  없이(`w_bytes` 등이 그냥 계속 쓰기만 한다) 직렬화하고 있었다 —
  `k_max_value_len`을 올린 뒤로는 이론상 필요한 최대 크기
  (8테이블×8값×1KiB ≈ 64KiB)가 그 버퍼를 훨씬 넘어, 실제로 그
  경계를 넘겨써 버퍼 뒤의 다른 정적 변수를 조용히 손상시켰다 —
  이 세션의 M41 자기테스트 값 두 개만으로도 그 순간이 실제로
  왔고, `[shell] cat ok=0`이라는 **완전히 무관해 보이는 회귀**로
  처음 드러났다(메모리 손상이 다른 전역 상태를 건드린 결과). 버퍼를
  32KiB로 늘리고, `persist_save()` 자신이 실제로 쓰기 **전에**
  필요한 전체 크기를 먼저 계산해 넘치면 아예 쓰지 않고 실패로
  처리하는 방어 코드를 추가했다 — 버퍼를 키우는 것만으로는 "더 큰
  상황에서 또 넘칠 수 있다"는 근본 문제를 없애지 못하기 때문이다.
- **실행 중 발견 4 — fs-protocol에 close가 없어 memfs의 열린 파일
  슬롯이 부팅 한 번 안에 바닥났다**(신규 OPEN-70): 위 발견 3을
  고치는 과정에서, cfgsrv가 상태가 바뀔 때마다(`persist_save()`)
  매번 새로 `vfs_open()`하면서 절대 close하지 않는다는 것을
  발견했다 — 사실은 fs-protocol(v3/v4) 자체에 open_file_id를
  반환하는 오퍼레이션이 애초에 없다(VFS를 거치는 모든 소비자가
  똑같이 겪는 근본 문제). memfs의 `k_max_open_files`(16)가 이
  라운드의 추가 `persist_save()` 호출 몇 번만으로 실제로 바닥나,
  cfgsrv 자신의 저장뿐 아니라 완전히 무관한 shell의 cat
  자기테스트까지(같은 고갈된 풀을 공유해서) 실패했다 — 즉시는
  64로 늘려 막고, 근본 수정(close 오퍼레이션 신설)은
  [open-items.md](open-items.md) OPEN-70으로 남겼다.
- **영향**: M42(컨트롤 프로토콜)의 `op_register`/`op_unregister`가
  이 클라이언트(`mc_reg_set_binary`/`mc_reg_delete_value`)를 그대로
  재사용할 수 있다 — 이번 라운드가 그 배선을 이미 검증해 뒀다.
  OPEN-70(fs-protocol close 부재)은 VFS를 쓰는 모든 서버에 영향을
  주는 더 큰 항목이라, 다음에 파일을 자주 열고 닫는 소비자가
  생기면 다시 마주칠 가능성이 높다.
