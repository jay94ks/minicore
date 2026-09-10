# 완료 보고: user-service-manager M41 — cfgsrv 기반 유닛 레지스트리

**대상 계획**: [user-service-manager.md](../plan/user-service-manager.md) §M41
**관련 결정**: [registry-decisions.md](../design/registry-decisions.md) ADR-197, ADR-215
**실행일**: 2026-09-10

## 완료한 것

1. `libs/mc/include/mc/cfgsrv_client.h`+`src/ipc/cfgsrv_client.c`
   (신규) — cfgsrv 레지스트리 프로토콜의 재사용 가능한 클라이언트
   (`open_table`/`create_table`/`get_value`/`set_value`(binary)/
   `delete_value`/`list_values`). procsrv의 M19 self-test가 인라인
   으로만 썼던 것을 처음으로 `libmc` 모듈로 뽑았다.
2. `libs/mc/include/mc/svcmgr_protocol.h`(신규) — ADR-196의
   `service_unit` 구조체(`mc_svcmgr_service_unit`).
3. svcmgr가 부팅 시 `@global/system/services`를 열고(없으면
   만들고) 비어 있으면 자기테스트 유닛 둘(`svc-b`가 `svc-a`에
   `depends_on`)을 등록, `list_values`+`get_value`로 읽어
   `depends_on`을 위상정렬해 순서대로 spawn+준비완료 대기한다.
4. `delete_value`로 `svc-b`를 지우고 `list_values`로 재조회해
   실제로 빠졌는지 확인한다.

## 계획 대비 범위 조정

- **"재부팅 후 확인"은 같은 부팅 안의 "등록→소비→삭제→재조회"로
  좁혔다** — cfgsrv의 저장 파일이 기본적으로 memfs에 떨어져
  (ADR-169 §결정2) 진짜 QEMU 재부팅을 거치면 사라진다. 디스크
  기반 경로로 바꾸는 것은 별개의 결정이라 이번 범위 밖이다.
- **`exec_path`(VFS 경로)는 저장/조회는 되지만 아직 읽지 않는다**
  — 등록된 유닛이 몇 개든 전부 M40과 같은 임베딩된 데모 ELF를
  실행한다. 서로 다른 실행 이미지를 VFS에서 읽으려면 `mc/fs_client.h`
  에 쓰기 클라이언트(그 이미지들을 VFS에 미리 심을 방법)까지 새로
  필요해 범위를 벗어난다고 판단했다.

## 실행 중 발견한 것

계획(ADR-197) 자체의 방향은 안 바뀌었지만, 실제로 cfgsrv를 문자열
값 이상으로 쓴 첫 소비자가 나타나며 진짜 버그 4건이 드러났다 —
그중 하나(발견 3)는 실제 메모리 손상이었다.

### 1. 페이지 버퍼 정렬 누락 → 즉시 커널 패닉

새 `mc/cfgsrv_client.c`의 정적 버퍼 3개(경로/키/값)에 `alignas`를
빠뜨려 `kernel/core/ipc/endpoint.cpp`의 4096바이트 정렬 강제에
걸려 "page_descriptor not page-aligned"로 즉시 패닉했다 —
`servers/procsrv/main.cpp`의 같은 용도 버퍼들이 이미 `alignas`를
쓰던 이유가 바로 이거였다. `_Alignas(MC_CFG_PAGE_SIZE)`를 추가해
고쳤다.

### 2. cfgsrv 값 저장 한도(256바이트) < service_unit(~616바이트)

`k_max_value_len`이 M19 시점 문자열 값만 염두에 두고 정해진
256바이트였다 — 넘는 값은 조용히 잘려 저장되고, 다시 읽으면
길이가 안 맞아 `load_units`가 매번 실패했다("load_units ok=0").
1024로 올렸다.

### 3. cfgsrv 영속화 버퍼가 실제로 넘쳐 메모리를 손상시켰다 (진짜 버그)

`persist_save()`가 전체 테이블 상태를 8KiB 정적 버퍼에 경계 검사
없이 직렬화하고 있었다. 발견 2로 `k_max_value_len`을 올린 뒤에는
이론상 필요한 최대 크기(8테이블×8값×1KiB ≈ 64KiB)가 이 버퍼를
훨씬 넘어, M41 자기테스트 값 두 개만으로 실제로 그 경계를 넘겨써
버퍼 뒤의 다른 정적 변수를 손상시켰다 — 이 손상은 **완전히
무관해 보이는 "[shell] cat ok=0" 회귀**로 처음 드러났다(다른 전역
상태가 오염된 결과였다). 버퍼를 32KiB로 늘리고, `persist_save()`
가 실제로 쓰기 전에 필요한 전체 크기를 먼저 계산해 넘치면 아예
쓰지 않는 방어 코드를 추가했다.

### 4. fs-protocol에 close가 없어 memfs 열린 파일 슬롯이 바닥났다 (OPEN-70 신규)

발견 3을 고치는 과정에서, cfgsrv가 `persist_save()`를 부를 때마다
매번 새로 `vfs_open()`하고 절대 close하지 않는다는 것을 발견했다
— 사실은 fs-protocol 자체에 close 오퍼레이션이 없다(모든 VFS
소비자가 겪는 근본 문제). memfs의 `k_max_open_files`(16)가 이
라운드가 늘린 `persist_save()` 호출 몇 번만으로 바닥나 cfgsrv
자신의 저장뿐 아니라 무관한 shell의 cat 자기테스트까지 실패시켰다
(같은 고갈된 풀 공유). 즉시는 64로 늘려 막고, 근본 수정(close
오퍼레이션 신설)은 OPEN-70으로 남겼다.

## 검증

x86_64 전체 재빌드 성공. QEMU에서 확인(순서대로): "[svcmgr]
services table open ok=1" → "[svcmgr] load_units ok=1" → "[svcmgr]
unit start name=svc-a" → "[svcmgr] unit start name=svc-b"(svc-a
**뒤에** 나옴 — depends_on 순서 확인) → "[svcmgr] delete_value
svc-b ok=1". "[shell] cat ok=1"(발견 3/4 수정 후 회귀 없음 재확인).
QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 117) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0) |

## 남겨 둔 것

user-service-manager.md M42(svcmgr 컨트롤 프로토콜 —
start/stop/restart/status/register)부터 계속 진행한다. `exec_path`
기반 서로 다른 실행 이미지 로딩, 진짜 QEMU 재부팅 간 영속성
(cfgsrv 저장 경로를 디스크 기반으로), fs-protocol close 오퍼레이션
(OPEN-70)은 모두 범위 밖으로 남긴다.
