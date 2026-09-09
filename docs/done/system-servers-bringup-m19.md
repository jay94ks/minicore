# 완료 보고: system-servers-bringup M19 — cfgsrv 설정 리포지터리

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M19
**관련 결정**: [registry-decisions.md](../design/registry-decisions.md)
ADR-060~064(레지스트리 도입/데이터 모델/권한 모델/신뢰 프로세스
보호/비밀 항목 raw 반환), ADR-169(M19 범위 좁힘)
**실행일**: 2026-09-09

## 완료한 것

### D0. 선행 스코핑 — ADR-169

M19를 시작하며 확인한 세 가지 진짜 갭에 대해 사용자에게
`AskUserQuestion`으로 물어 확정했다: (1) procsrv의 계정 모델에
`gid` 개념이 아예 없어 group 권한 비트는 이번 라운드에서 검증하지
않는다(owner/other만), (2) cfgsrv가 실제로 VFS 파일에 영속화한다
(사용자가 더 충실한 쪽을 선택 — M18의 ELF 로더 결정과 같은 패턴),
(3) `reg_op` 9종을 전부 구현한다(역시 더 충실한 쪽). 구현 중 네
번째 갭도 발견해 별도로 물었다 — `open_table`이 진짜 커널
`object_kind::reg_table` 핸들을 `message.handles[0]`으로 반환하려면
"살아있는 프로세스가 런타임에 새 커널 객체를 만드는 syscall"이
필요한데 아직 없다(VFS/memfs의 `open_file_id`도 실은 이 캡을 피해
단순 정수로 설계된 선례). 사용자는 이번엔 권장 단순화(프로토콜-레벨
정수 핸들)를 선택했다. 네 결정 모두 registry-decisions.md ADR-169로
기록했고, docs/spec/registry.md도 함께 갱신했다(§5.1 wire 매핑,
§7 실제 저장 경로).

### D1. `servers/cfgsrv` — 새 서버

`@스키마/A/B/table` 주소 체계(스키마 생략 시 호출자 사용자명),
타입 있는 키-값 사전(string/int64/boolean/binary), Unix RWX(owner/
other, group은 §결정1로 항상 무시) 권한 모델을 구현했다. `reg_op`
9종(open/create/delete_table, list_children, get/set/delete_value,
list_values, set_permissions)을 전부 구현했고, 자신의 전체 테이블
상태를 VFS 경로(`/sys/etc/registry.dat`, 기본 라우팅으로 memfs)에
쓰기 오퍼레이션마다 다시 직렬화해 저장하며 부팅 시 복원을
시도한다(memfs가 seek/truncate를 지원하지 않아 직렬화 포맷 맨
앞에 `payload_len`을 둬 이전 라운드의 더 길었던 내용이 남아 있어도
정확히 그만큼만 읽는다).

### D2. `servers/procsrv` — cfgsrv 클라이언트 + 왕복/권한 검증

initrun의 스폰 시점 캐패빌리티 주입에 cfgsrv 의존이 추가돼
(`depends=vfs,cfgsrv`) handle 3으로 cfgsrv를 받는다. root 동등
uid(0)로 `@global/test/settings` 테이블을 만들고 값을 쓰고 다시
읽는 왕복을 확인한 뒤, 소유자가 아닌 uid(test=1000)가 기본
비공개(other_rwx=0) 테이블을 열 수 없음을 확인하고, 소유자가
`set_permissions`로 other에 읽기 권한을 열어 준 뒤에는 같은
비소유자가 읽을 수 있음을 확인한다 — 권한 모델(owner/other RWX)이
실제로 두 방향 다 동작함을 증명한다. 나머지 오퍼레이션
(list_values/list_children/delete_value/delete_table)도 한 번씩
행사해 9종 전부가 동작함을 확인한다.

### D3. `servers/CMakeLists.txt`, `tools/mkbootdisk.py` 배선

부트 디스크 서비스 목록에 cfgsrv를 추가했다(순서
memfs→...→vfs→cfgsrv→procsrv→login — cfgsrv가 자신을 영속화하려면
vfs가 먼저 있어야 하고, procsrv는 vfs+cfgsrv 둘 다에 의존한다).
cfgsrv는 ADR-063/074의 신뢰 프로세스 보호 대상이라 `--trusted=cfgsrv`
로 표시했다(초기 부팅 매니페스트의 `grant_trusted`는 M14부터 이미
있던 것을 그대로 재사용 — 새 kernel 변경 불필요, `trusted` 플래그와
`object_kind::reg_table` 열거값 자체는 이미 M1~M8 시절부터 미리
예약돼 있었다는 것을 이번에 확인했다).

## 검증 중 발견하고 고친 버그 — ELF 로더의 세그먼트 경계 공유

첫 QEMU 부팅에서 `[elf_loader] map_failed page_vaddr=0x10004000 err=1`
(already_mapped)로 cfgsrv의 스폰이 조용히 실패했다(initrun이 실패를
로그만 남기고 다음 서비스로 넘어가, "[cfgsrv] persist load found=0"
같은 자기 로그가 전혀 안 보이는 것으로 알아챘다). 원인: cfgsrv의
전역 `constexpr const char* k_persist_path = "...";`가 ld.lld에서
`.data.rel.ro`에 담기며 별도의 8바이트 PT_LOAD 세그먼트로 분리됐는데,
그 세그먼트가 `.rodata` 세그먼트와 같은 4KiB 페이지(0x10004000)를
공유했다 — `kernel/arch/x86_64/elf_loader.cpp`가 세그먼트별로
독립적으로 `map_page`를 호출해(이미 매핑된 페이지를 다시 매핑하려는
경우를 감지하지 않음) 같은 페이지를 두 번 매핑하려다 실패했다.
procsrv/vfs 등 기존 서버들은 섹션 크기가 우연히 이 경계를 넘지
않아 지금까지 드러나지 않았던 잠재적 결함이다. 커널의 elf_loader를
고치는 대신(모든 프로세스의 로딩 경로에 영향을 주는 더 큰 변경)
cfgsrv 쪽에서 트리거를 없앴다 — 문자열 리터럴을 가리키는 전역
포인터 변수를, 호출 지점에서 주소를 직접 계산하는 함수
(`const char* k_persist_path()`)로 바꿔 그 포인터 자체가 생기지
않게 했다. (일반적인 elf_loader의 "세그먼트 간 페이지 공유" 처리는
여전히 잠재적 결함으로 남아 있다 — 다른 서버가 우연히 같은 경계를
다시 넘으면 재발할 수 있다.)

또한 struct 필드 초기화 관례(가변 길이 `memset`/`memcpy` 금지)를
지키다가 `kv_entry::key` 필드의 `= {}` 기본 초기화를 실수로 함께
지웠던 적이 있다 — 그 결과 `table_entry`가 컴파일 타임에 완전히
상수 초기화되지 못해(전역 배열 `g_tables`가 `.bss`가 아니라
`.init_array`를 통한 런타임 생성자 호출을 요구하게 됨) 바로 위의
버그와 같은 대칭적인 원인(전역 constexpr 초기화 요구사항,
ADR-118)으로 또 다른 세그먼트 분리를 유발했다는 것도 디스어셈블로
확인해 되돌렸다.

## 검증 결과 (QEMU 실측)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64 minicore_bootdisk_image
tools/smoke-test-x86_64.sh       # 76개 전부 PASS(M18의 71개 + 신규 5개) — 2회 연속 재확인
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS(회귀 없음)
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS(회귀 없음)
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS(회귀 없음)
```

실측 로그(발췌):

```
[cfgsrv] persist load found=0
[su-target] guest open outside denied=1
[cfgsrv] persist write ok=1
[su-target] guest open inside ok=1
[cfgsrv] persist write ok=1
[procsrv] cfgsrv roundtrip ok=1
[procsrv] cfgsrv permission denied before grant=1
[cfgsrv] persist write ok=1
[procsrv] cfgsrv permission granted after chmod=1
[cfgsrv] persist write ok=1
[cfgsrv] persist write ok=1
[procsrv] cfgsrv full protocol ok=1
[login] auth ok=1
[login] su delegated ok=1
[login] su denied ok=1
```

- **확인함**: procsrv가 cfgsrv에 값을 쓰고 다시 읽는 왕복이
  성공한다(`roundtrip ok=1`) — 계획이 명시한 M19의 핵심 검증 목표.
- **확인함**: 권한 모델이 두 방향 다 실제로 동작한다 — 소유자가
  아닌 uid는 기본 비공개 테이블을 열 수 없고(`permission denied
  before grant=1`), 소유자가 `set_permissions`로 열어 준 뒤에는
  열 수 있다(`permission granted after chmod=1`).
- **확인함**: `reg_op` 9종 전부(open/create/delete_table,
  list_children, get/set/delete_value, list_values,
  set_permissions)가 최소 한 번씩 성공적으로 행사됐다(`full protocol
  ok=1`).
- **확인함**: cfgsrv가 VFS로 실제 쓰기 IPC 왕복을 수행한다(`persist
  write ok=1`, 여러 번). **알려진 한계**: 이 프로젝트엔 아직 쓰기
  가능한 디스크 백엔드 FS 서버가 없다(fat32/ext4는 M16에서
  읽기전용으로 범위가 좁혀졌고, `/sys/etc/registry.dat`는 기본
  라우팅으로 memfs — 순수 인메모리라 재부팅을 넘어서는 진짜
  내구성은 아직 없다). 이번 라운드는 "VFS로 실제 쓰고 다시 읽는"
  코드 경로 자체가 동작함을 증명했을 뿐, 재부팅 후 로드까지는
  이 단일 QEMU 부팅 스모크 테스트로 증명하지 못한다(구조적으로
  memfs가 재부팅마다 비므로 현재는 증명 자체가 불가능하다).
- **이번 라운드에서 발견하고 고친 버그**: 위 절 참고(ELF 로더의
  세그먼트-페이지 공유 미처리, 그리고 그걸 다시 유발한 struct
  초기화 되돌림 실수).
- **알려진 단순화(M19 범위 밖으로 명시적으로 남긴 것)**: group
  권한 비트 검증(procsrv에 gid 개념 자체가 없다), `special_bits`의
  의미(registry.md 자신도 미정), 변경 알림/트랜잭션 지원(registry.md
  자신도 미정), 진짜 커널 `reg_table` 객체 + 런타임 객체 생성
  syscall(ADR-169 §결정4), `owner_uid`/`group_gid`가 `@global/system/
  users`/`groups` 테이블과 실제로 연동되는 계정 프로비저닝(ADR-079,
  procsrv 상세 설계 이후).

## 다음 단계

M19는 이것으로 완료됐다. 다음 마일스톤은
[system-servers-bringup.md](../plan/system-servers-bringup.md) §M20
(libc/POSIX 포팅 + 로그인 후 셸) — 이 계획의 마지막 마일스톤이다.
