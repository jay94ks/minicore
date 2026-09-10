# M43 완료 — 계정별 유저 서비스 인스턴스 (스트레치)

[user-service-manager.md](../plan/user-service-manager.md) §M43(스트레치
— "이 마일스톤이 착수되지 않거나 실패해도 M40~M42의 성과는 독립적으로
유효하다"고 계획 문서 자신이 명시한 마일스톤). 관련 ADR:
[ADR-217](../design/security-model.md)(스폰 시점 badge 오버라이드),
[ADR-218](../design/security-model.md)(계정별 유저 서비스 위임),
[ADR-219](../design/boot-and-drivers.md)(svcmgr 확장 — scope/로그인
감시 스레드/주소 지정).

## 방향 결정

계획 문서 자신이 "svcmgr가 임의 uid로 프로세스를 spawn할 권한을
procsrv로부터 어떻게 받는가"를 착수 시점의 새 미결정 항목으로
예고해 뒀다(태생적 권한 vs su/sudo류 위임). 사용자가 명시적으로
두 계층으로 구분했다:

- **시스템 전역으로 설치된 유저 서비스**: svcmgr 자신의 태생적
  권한으로 계속 구동(M40~M42와 완전히 동일, 변경 없음).
- **계정 전용 서비스**: 그 계정이 **영구 위임을 등록했다가 철회할
  수 있는** 방식으로, 그 계정 자신의 권한 범위 안에서만 동작.

## 무엇을 했는가

### 1. ADR-217 — 스폰 시점 badge 오버라이드 (kernel/init/initrun)

procsrv에 svcmgr만 부를 수 있어야 하는 민감한 새 오퍼레이션
(계정 위임 확인 후 spawn)이 필요했다. 기존 procsrv 오퍼레이션들
(`adopt_orphans` 등)의 "호출자가 자기 pid를 자기주장"(OPEN-67)
모델은 이 경우 진짜 권한 상승 벡터가 된다 — 그래서 처음으로
**badge**(ipc.md가 이미 설계해 뒀지만 M6~M42 내내 아무도 실제로
세팅해 쓴 적이 없던 필드)를 스폰 시점 캐패빌리티 주입에 연결했다.

- `mc_handle_transfer`(`libs/mc/include/mc/syscall.h`)에
  `badge_override`/`has_badge_override` 필드 추가.
- `kernel/arch/x86_64/process_ops.cpp`의 스폰 시점 `inherited_handles`
  주입 루프가 이 필드를 `create_proxy`(커널이 이미 지원하던 파라미터)
  로 그대로 전달.
- `init/initrun/main.cpp`에 "svcmgr가 받는 procsrv 핸들"이라는
  조합 하나만 하드코딩된 예약 badge(`MC_PROCSRV_SERVICE_DELEGATION_BADGE`)
  를 스탬핑하는 특수 케이스 추가 — 일반 `--depends=` 문법은 확장하지
  않았다(YAGNI).
- procsrv가 이 badge를 실제로 읽으려면 `sys_recv`의 badge 반환값
  자체가 필요한데, 실행 중 발견: **`kernel/arch/x86_64/syscall.cpp`의
  `MC_SYSCALL_IPC_RECV` 케이스가 M6부터 이 값을 항상 버려 왔다**
  (`kern::ipc::sys_recv`가 `result<uint64_t, ipc_error>`로 badge를
  이미 계산해 반환하는데도, raw syscall 디스패처가 상태 코드만
  돌려주고 그 값을 절대 유저랜드에 넘기지 않았다). a3(지금까지
  MC_SYSCALL_IPC_RECV에서 안 쓰던 슬롯)를 badge 출력 포인터로
  써서 고쳤다 — 기존 호출자(a3=0) 동작은 그대로다.

### 2. ADR-218 — 계정별 유저 서비스 위임 (procsrv/cfgsrv)

su/sudo(ADR-093)의 위임 테이블을 재사용하지 않았다 — su는 "임의
명령 실행"이라는 훨씬 넓은 권한이고, 이건 "내가 등록한 유저 서비스
유닛만" 스폰하는 훨씬 좁은 권한이다. 새 테이블
`@<계정명>/system/service-delegate`(키="svcmgr" 하나뿐, 값=
`{u64 granted_at, u8 mode}`, v1은 mode=permanent만 지원)를 뒀다.

- **자가서비스 grant/revoke**: 새 오퍼레이션 없이, 그 계정 자신이
  cfgsrv의 기존 `set_value`/`delete_value`를 직접 호출한다(자기가
  소유한 테이블이라 cfgsrv의 기존 권한 검사를 그대로 통과).
- procsrv에 새 오퍼레이션 2개(`mc/procsrv_protocol.h` label=16/17,
  ADR-195 마크업 방법론): `op_poll_login_event`(로그인 성공마다
  채워지는 원형 큐를 svcmgr가 비블로킹으로 폴링, OPEN-67과 같은
  이유), `op_spawn_delegated_unit`(ADR-217의 badge로 호출자 확인
  →위임 조회→procsrv 자신이 컴파일 시점에 심어 둔 데모 ELF를
  spawn — svcmgr가 넘긴 elf_data 포인터는 svcmgr 자신의 주소공간을
  가리켜 procsrv가 역참조할 수 없으므로, procsrv도 같은 ELF를
  `servers/procsrv/CMakeLists.txt`로 직접 심었다).
- guest/jail 계정은 이 위임도 무효(ADR-093 §7과 같은 원칙).
- 취소는 미래의 신규 spawn만 막는다 — 이미 떠 있는 인스턴스는
  정상 종료까지 계속 실행(ADR-094 원칙 재사용).
- **명시적 한계**: 스폰된 프로세스는 실제 커널/badge 수준의 그
  계정 신원을 받지 않는다 — procsrv/svcmgr의 부기에만 "이 계정
  몫"이라고 기록된다(이 프로젝트의 uid 모델 자체가 아직 그 수준
  이라 — M18/M20이 이미 로그인 세션도 같은 수준으로 좁혀 둔 것의
  연장). → **OPEN-72** 신규.

### 3. ADR-219 — svcmgr 확장 (scope/로그인 감시/주소 지정)

- `mc_svcmgr_service_unit`에 `scope`(system 기본/per_account) 필드
  추가 — per_account 유닛은 템플릿일 뿐, 부팅 시 시작하지 않는다.
- svcmgr의 첫 멀티스레드 사용(M37 `mc_thread_create`) — 로그인
  감시 스레드가 `op_poll_login_event`를 반복 호출해, 로그인된
  계정마다 per_account 유닛 전부에 대해 `op_spawn_delegated_unit`
  을 시도한다(위임 없으면 조용히 스킵, 에러 아님).
- 컨트롤 프로토콜 주소 지정을 `"<유닛명>@<계정명>"` 형식으로
  확장(`@` 없으면 기존 system-scope 그대로, 하위 호환) —
  `runtime_unit`의 키를 (유닛명, 계정명) 페어로 확장.
- `g_runtime`이 이제 메인 스레드와 로그인 감시 스레드 양쪽이
  건드려 스핀락(libk) 추가.

### 4. 검증 프로그램

- `userland/user-service-delegate-test`(신규): "test"/"root" 두
  계정을 대신해 자가서비스로 위임을 등록.
- `servers/login`: 자동 로그인이 "test" 하나뿐이었던 것을 "root"
  로도 한 번 더 로그인하도록 확장(계획 원문의 "계정 두 개" 검증
  목표).
- `servers/svcmgr`: 자기테스트 유닛에 per_account 템플릿
  "svc-u"(의존 없음) 추가.
- `userland/svcmgr-ctl-test`: M42의 기존 검증 뒤에 "svc-u@test"/
  "svc-u@root"의 상태를 폴링(최대 64회, `mc_yield` — 로그인 감시
  스레드가 메인 IPC 루프와 별도로 도는 비동기 완료라 재시도가
  필요하다)해 둘 다 `running=1`이고 kernel thread 핸들이 서로
  다름(=진짜 독립된 프로세스)을 확인.

## 실행 중 발견한 진짜 버그/설계 오류 3건

1. **`sys_recv`의 badge 반환값이 M6부터 버려지고 있었음** —
   위 §1 참고. badge라는 위조 불가능한 호출자 식별 메커니즘이
   ipc.md에 이미 설계돼 있었지만, 이걸 처음 실제로 쓰려던 M43이
   되어서야 raw syscall 계층 자체가 그 값을 유저랜드에 절대 넘긴
   적이 없었다는 걸 발견했다.
2. **`@global/system/service-delegates/<계정>` 경로가 애초에
   작동할 수 없었음** — cfgsrv의 `normalize_path`/`schema_matches`
   가 `@global/...`의 스키마를 항상 문자열 "global" 자체로 고정
   취급해, `caller_uid != 0`인 계정은 그 아래에 `CREATE_TABLE`을
   절대 통과시킬 수 없다(uid=0/root만 가능 — 의도된 설계). 자가서비스
   grant가 성립하려면 계정 자신의 스키마(`@<계정명>/...`)를 써야
   했다 — `@global/*` 아래 테이블이 그 전까지 전부 uid=0/root가
   만든 것뿐이라(예: `@global/system/services`, ADR-197) 이 제약이
   한 번도 드러날 기회가 없었다.
3. **svcmgr의 두 번째 스레드가 `g_runtime`을 동시에 건드릴 수
   있게 됨** — M42까지 svcmgr는 단일 스레드라 락이 필요 없었다.
   로그인 감시 스레드를 추가하며 메인 IPC 루프(컨트롤 프로토콜
   처리)와 동시에 `g_runtime`을 읽고 쓸 수 있게 돼 스핀락이
   필요해졌다(ADR-219 §근거가 미리 예상해 둔 것).

## 검증 (QEMU, x86_64)

핵심 확인 로그:

```
[user-service-delegate-test] grant test ok=1
[user-service-delegate-test] grant root ok=1
[login] auth ok=1
[login] second account (root) login ok=1
[svcmgr] login watcher thread ok=1
[svcmgr] login event account=test
[svcmgr] per_account spawn ok name=svc-u account=test
[svcmgr] login event account=root
[svcmgr] per_account spawn ok name=svc-u account=root
[svcmgr-ctl-test] status svc-u@test running=1
[svcmgr-ctl-test] status svc-u@root running=1
[svcmgr-ctl-test] per-account instances distinct=1
```

`tools/smoke-test-x86_64.sh`에 M43 어서션 11개 추가, 전체 스모크
스위트(기존 어서션 전부 포함, 135개) PASS(exit 0). 추가로 SMP/NUMA/
AVX/net 4개 회귀 스위트 전부 PASS(exit 0, FAIL 없음) — 커널
(`syscall.cpp`/`process_ops.cpp`)·`mc_handle_transfer` ABI 확장이
다른 IPC/스폰 소비자에 회귀를 만들지 않았음을 확인했다.

## 범위 밖으로 남긴 것

- **OPEN-71**: 위임 기간 모드는 permanent 하나뿐 — TTL/계정
  기본값 모드는 없다.
- **OPEN-72**: 스폰된 프로세스는 실제 커널/badge 수준의 계정
  신원을 받지 않는다.
- **OPEN-73**: 계정별 인스턴스에는 ADR-193의 준비완료 핸드셰이크가
  연결돼 있지 않다(procsrv가 spawn하므로 그 프록시 핸들을 svcmgr
  에게 넘기려면 `sys_reply`의 `handles[]` 위임까지 얹어야 해서
  범위를 넘었다).
- OPEN-67(procsrv 오퍼레이션 전반의 자기주장 caller_pid/비블로킹
  wait)은 이번에 아주 좁은 한 조각(`op_spawn_delegated_unit` 하나)
  만 badge로 실제 검증하게 됐을 뿐, 전반적으로는 여전히 열려 있다.
- exec_path(VFS 경로)는 여전히 안 읽는다 — per_account 유닛도
  전부 같은 임베딩된 데모 ELF를 실행한다(M41부터 이어지는 제약).

## 다음

user-service-manager.md는 이제 M40~M43 전부 완료됐다 — 이 계획
문서에는 더 이상 다음 마일스톤이 없다.
