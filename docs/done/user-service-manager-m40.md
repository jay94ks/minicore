# 완료 보고: user-service-manager M40 — servers/svcmgr 골격 + 재부모화 완성

**대상 계획**: [user-service-manager.md](../plan/user-service-manager.md) §M40
**관련 결정**: [boot-and-drivers.md](../design/boot-and-drivers.md) ADR-192, ADR-193, ADR-196, ADR-214
**실행일**: 2026-09-10

## 완료한 것

1. 새 서버 `servers/svcmgr` — `libk`+`libmc`만 링크한 순수
   minicore 네이티브 서버(musl 불필요, ADR-006/007/132). initrun이
   `--service=` 목록의 마지막 항목으로 spawn한다.
2. procsrv 새 wire op `adopt_orphans`(label=15,
   `mc/procsrv_protocol.h`) — svcmgr가 자기 pid를 확정한 뒤 부르면,
   M27이 `parent_pid=k_parent_none`으로 잠정 등록해 둔 고아들을
   실제로 svcmgr에게 재부모화한다. `reparent_children()`에
   `exclude_pid`를 추가해 svcmgr 자신은 재부모화 대상에서 뺐다.
3. `libs/mc/include/mc/lifecycle_client.h`(신규) —
   `mc_signal_ready()`/`mc_wait_ready()`. ADR-193의 준비완료 신호를
   처음 실제로 구현했다.
4. `userland/svcmgr-demo-unit`(신규) — 하드코딩된 데모 유닛 하나.
   svcmgr가 자신의 컴파일 시점 데이터로 심어(`tools/bin2c.py`)
   VFS 없이 곧바로 spawn한다.

## 실행 중 발견한 것

계획(ADR-192/193/196) 자체의 방향은 바뀌지 않았지만, 실제로 이
경로를 처음 쓰는 소비자가 나타나며 진짜 버그 4건이 드러났다.

### 1. spawn 시점 endpoint 프록시가 CAN_SEND만 있었다

`process_spawn(create_endpoint=true)`가 돌려주는
`out_endpoint_proxy_handle`은 M22부터 `k_right_can_send`만
부여했다(부모가 자식에게 먼저 Call을 거는 wait-target 패턴만
가정). ADR-193의 준비완료 신호는 방향이 반대(자식이 Call, 부모가
Recv)라 `k_right_can_recv`가 없으면 막힌다. `k_right_can_send |
k_right_can_recv` 둘 다 주도록 고쳤다(기존 패턴은 그대로 계속
동작).

### 2. `mc/*_client.h`에 `extern "C"`가 하나도 없었다

`vfs_client.h`/`fs_client.h`/`console_client.h`/`ps2_client.h`/
`procsrv_client.h`가 전부 `extern "C"` 가드 없이 C 링크 심벌을
선언했다 — 지금까지 아무 C++ 서버도 이 함수들을 직접 호출한 적이
없어(각자 자체 syscall 트램폴린만 쓰거나 구조체/상수만 참조)
드러나지 않았다. svcmgr(C++)가 `mc_getpid`/`mc_adopt_orphans`를
직접 부르며 이름 맹글링 링크 에러로 처음 걸렸다. 다섯 헤더 전부에
`#ifdef __cplusplus extern "C" { ... }`를 추가했다.

### 3. `--depends=`에 이름을 많이 나열하면 조용히 전부 무시된다

처음엔 ADR-196의 "의존관계는 기존 커널 서버 전체"를 문자 그대로
15개 서비스 이름을 `--depends=`에 나열했다. 두 문제가 겹쳤다:
`MC_MAX_SPAWN_INHERITED_HANDLES`(=4)를 넘는 이름은 핸들을 못 받고,
`init/initrun/main.cpp`의 depends= 파싱 버퍼(96바이트)보다 그
문자열이 길어(15개+콤마 >100자) 파싱 자체가 실패해 **procsrv
핸들조차 못 받았다** — svcmgr의 `self_register`가 존재하지 않는
핸들로 IPC를 걸어 즉시 실패했다. 실제로는 스폰 순서 보장(이미
`--service=` 목록의 마지막 줄이라는 사실로 충족)과 핸들 상속(별개
메커니즘)을 혼동한 것이었다 — `--depends=svcmgr:procsrv` 하나로
줄여 해결했다.

### 4. procsrv 자신도 procsrv를 몰랐다

M40의 검증 목표("커널 서버들의 parent_pid가 svcmgr로 바뀜")를
확인하려 했으나, VFS/devmgr/cfgsrv 등 어떤 커널 서버도 procsrv에
`self_register`한 적이 없었다(procsrv 자신의 pid=1 등록조차
self-test 전용 코드 경로에만 있었다) — 실제 부팅 경로에는
재부모화할 진짜 대상이 하나도 없었다. procsrv가 `_start()` 맨
앞에서 무조건 자기 자신을 pid=1로 등록하도록 고쳐, 최소한 하나의
관찰 가능한 재부모화 대상을 만들었다.

## 검증

x86_64 전체 재빌드 성공. QEMU에서 확인(순서대로): "[svcmgr]
self_register ok=1" → "[svcmgr] adopt_orphans ok=1" → "[svcmgr] demo
unit spawn ok=1" → "[svcmgr] demo unit ready ok=1". QEMU 5개 회귀
스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 114) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0) |

## 남겨 둔 것

user-service-manager.md M41(cfgsrv 기반 유닛 레지스트리 — 정적
목록을 실제 테이블로 대체)부터 계속 진행한다. svcmgr는 아직 자기
own endpoint 위에서 어떤 오퍼레이션도 처리하지 않는다(무한
recv/reply만 반복 — M42가 실제 컨트롤 프로토콜을 채운다). 커널
서버 자체가 procsrv에 self_register하도록 만드는 것은 이 계획의
범위 밖으로 남긴다(발견 4 참고 — 유저 서비스는 procsrv의 fork/exec
경로를 거쳐 spawn될 가능성이 높아 이 문제를 자연히 피한다).
