# 실행 계획: 유저 서비스 관리자 데몬(svcmgr) (M40~M43)

**관련 결정**: [boot-and-drivers.md](../design/boot-and-drivers.md)
ADR-192(OPEN-51 방향 전환 — "커널 서버"는 initrun 직접 실행, "유저
서비스"는 별도 데몬), ADR-193(준비완료 신호 통일), ADR-196(svcmgr
설계), [registry-decisions.md](../design/registry-decisions.md)
ADR-197(유닛 레지스트리 스키마), [build-system.md](../design/build-system.md)
ADR-195(와이어 프로토콜 마크업+추출 도구), [procsrv.md](../spec/procsrv.md)

**선행 완료 전제**: [real-libc-syscall-layer.md](real-libc-syscall-layer.md)
**M27**(procsrv 실제 프로세스 테이블 + 범용 `OP_WAIT`/`OP_KILL` +
ADR-192 §결정3의 재부모화 **메커니즘**, 대상은 아직 `parent_pid=0`
잠정 처리)까지 완료돼 있어야 한다 — 이 계획은 그 잠정 처리를 실제
대상(svcmgr)으로 교체하는 것부터 시작한다. M28 이후(musl syscall
계층)는 이 계획과 무관하다 — svcmgr는 순수 minicore 네이티브
서버(`libk`+`libmc`만 링크, ADR-006/007/132의 "2a" 계층)이므로 musl
포팅과 독립적으로 병행할 수 있다.

## 배경

ADR-131 §결정7이 "systemd류 초기 프로세스"의 존재만 예고하고
정체성·책임 범위를 OPEN-51로 미뤄 뒀다. ADR-192가 그 방향을
"커널 서버(하드코딩, initrun 직접 실행)"와 "유저 서비스(더 동적인
집합)"의 두 계층으로 분리해 OPEN-51을 해소했지만, "유저 서비스를
실제로 어떻게 기술·등록·시작·정지하는가"는 ADR-196/197로 설계만
해 뒀을 뿐 아직 한 줄도 구현되지 않았다. 이 계획이 그 구현을 다룬다.

## M40. `servers/svcmgr` 골격 + 정적 유닛으로 재부모화 완성

- **구현**: [ADR-196](../design/boot-and-drivers.md)의 §결정1(신설
  서버)·§결정5(재부모화 대상 확정)를 구현한다.
  1. 새 서버 `servers/svcmgr` — `libk`+`libmc`만 링크(순수
     minicore 네이티브, musl 불필요). initrun이 모든 "커널 서버"를
     기동한 뒤 **마지막으로** spawn한다(`servers/CMakeLists.txt`의
     `--service=svcmgr=...` 항목 추가, 의존관계는 기존 커널 서버
     전체).
  2. 이 마일스톤은 유닛 목록을 아직 레지스트리에서 읽지 않는다 —
     **하드코딩된 데모 유닛 하나**(예: `hello` 서비스 하나)만 두고,
     ADR-193의 준비완료 신호(spawn 시점 전용 endpoint의
     `k_service_ready_label` Call)로 기다렸다가 다음으로 넘어가는
     패턴이 실제로 동작하는지부터 증명한다.
  3. M27이 잠정적으로 `parent_pid=0`으로 둔 재부모화 대상을
     **svcmgr 자신의 pid**로 교체한다 — initrun이 사라지는 시점에
     procsrv가 남은 커널 서버들의 `parent_pid`를 svcmgr로 갈아치우는
     실제 호출을 추가한다(메커니즘 자체는 M27이 이미 만들어 뒀다,
     대상만 매개변수로 넘긴다).
- **목표**: QEMU 부팅 로그로 순서를 확인한다 — 커널 서버들
  기동→svcmgr spawn→initrun 사라짐→svcmgr가 데모 유닛 spawn+준비완료
  대기→통과. 이후 `/sys/proc`(또는 디버그 로그)로 커널 서버들의
  `parent_pid`가 더 이상 `0`이 아니라 svcmgr의 pid임을 확인한다.

## M41. cfgsrv 기반 유닛 레지스트리 — 정적 목록을 실제 테이블로 대체

- **구현**: [ADR-197](../design/registry-decisions.md)의 스키마를
  실제로 쓴다.
  1. svcmgr가 부팅 시 하드코딩된 목록(M40) 대신 `@global/system/services`
     테이블을 `list_values`+`get_value`(기존 `reg_op`, cfgsrv 클라이언트는
     `libmc`에 이미 있는 것을 재사용 — 없는 함수만 추가, ADR-183
     §결정4와 같은 원칙)로 읽는다.
  2. `depends_on` 그래프를 단순 위상정렬해 시작 순서를 정한다 —
     순환은 감지해 그 서비스들만 스킵+로그(ADR-196 §결정3, 해소하지
     않음).
- **목표**: `@global/system/services`에 서비스 둘(B가 A에
  `depends_on`)을 `set_value`로 등록하고 재부팅해, svcmgr가 A를
  먼저 시작하고 A의 준비완료 신호를 받은 뒤에야 B를 시작함을 QEMU
  로그로 확인한다. 하나를 `delete_value`로 지우고 재부팅해 더 이상
  시작되지 않음도 확인한다.

## M42. svcmgr 컨트롤 프로토콜 — start/stop/restart/status/register (ADR-195 방법론 적용)

- **구현**: [ADR-196](../design/boot-and-drivers.md) §결정7의
  오퍼레이션 목록을 실제 와이어 프로토콜로 만든다.
  1. **[ADR-195](../design/build-system.md)의 순서를 그대로 따른다**
     — `libmc/include/mc/svcmgr_protocol.h`에 `@wire-op` 마크업과
     함께 `op_list`/`op_status`/`op_start`/`op_stop`/`op_restart`/
     `op_register`/`op_unregister`를 먼저 작성하고,
     `tools/gen-wire-docs.py`로 추출·확인한 뒤에야 구현한다(M27이
     첫 적용 사례였다면 이번이 두 번째).
  2. `op_stop`/`op_restart`는 기존 procsrv `OP_KILL`(ADR-178/186의
     `sys_process_kill`/`sys_signal_send`)을 그대로 호출한다 — 새
     종료 메커니즘을 만들지 않는다.
  3. `op_register`/`op_unregister`는 `@global/system/services`
     테이블에 대한 `set_value`/`delete_value`를 그대로 호출한다
     (M41이 이미 그 클라이언트를 만들어 뒀다) — cfgsrv 자신의
     ADR-062 권한 검사(owner=root만 write)에 그대로 의존하고, svcmgr는
     별도로 권한을 검사하지 않는다(ADR-197 §결정3).
- **목표**: 새 컨트롤 클라이언트(별도 최소 테스트 프로그램 또는
  셸의 새 빌트인, 착수 시점에 확정)가 `op_stop`으로 실행 중인 데모
  서비스를 정지시키고 `/sys/proc`으로 실제 종료를 확인한 뒤,
  `op_start`로 다시 띄워 준비완료 왕복이 다시 성립함을 확인한다.
  `op_register`로 새 유닛을 추가한 뒤 재부팅해 그 유닛이 실제로
  자동 시작됨을 확인한다(cfgsrv의 기존 VFS 영속화, M19가 이미
  검증해 둔 경로를 그대로 탄다).

## M43. 계정별 유저 서비스 인스턴스 (스트레치)

- **구현**: systemd의 `user@.service`에 대응 — 로그인한 계정마다
  독립된 서비스 인스턴스를 만드는 것. procsrv가 이미 로그인
  시점에 "다른 신원으로 새 프로세스 생성"을 하는 경로(ADR-088
  §8.3)가 있으므로, svcmgr가 **그 계정의 세션 프로세스로부터
  위임받아** 같은 경로로 인스턴스를 spawn하는 형태를 검토한다.
- **주의(사전 진단)**: "svcmgr가 임의 uid로 프로세스를 spawn할 수
  있는 권한을 procsrv로부터 어떻게 받는가"는 아직 어떤 ADR도 답하지
  않았다 — login/procsrv 자신처럼 처음부터 그 권한으로 태어나는
  것과 su/sudo류 위임(ADR-093)을 받는 것 중 어느 쪽이 맞는지가 이
  마일스톤 착수 시점의 새 미결정 항목이 될 가능성이 높다. **이
  마일스톤이 착수되지 않거나 실패해도 M40~M42의 성과는 독립적으로
  유효하다** — 그래서 스트레치로 분리했다.
- **목표**: 계정 두 개로 각각 로그인해, 같은 이름의 유저 서비스가
  각 계정 세션마다 독립된 프로세스로(공유 없이) 뜨는 것을 확인한다.

## 포함하지 않는 것 (이 계획 이후로 명시적으로 미룸)

- **재시작 정책**(`Restart=on-failure` 등) — ADR-196 §결정6, v1은
  죽으면 그대로 끝이다.
- **소켓 활성화, 타이머/cron 대응, 리소스 제한(cgroups 대응), 유닛
  템플릿** — ADR-196이 이미 범위 밖으로 명시.
- **의존성 순환의 자동 해소** — 감지만 하고 해당 서비스만 스킵한다
  (M41).
- **마운트 계획의 boot-파라미터화**(ADR-192 §결정4 나머지) — 이
  계획과 무관한 별개 라운드 대상.
- aarch64 이식 — 여전히 별도 방향.

## 검증 방법

기존 계획들과 같은 방식 — QEMU 부팅 로그로 확인 가능한 마일스톤별
완료 기준을 두고, `tools/smoke-test-x86_64.sh`에 확인 문자열을
마일스톤마다 추가한다. svcmgr는 musl과 무관하므로
[real-libc-syscall-layer.md](real-libc-syscall-layer.md)의 어떤
마일스톤 진행 상황과도 독립적으로 검증할 수 있다 — 다만 M40이
전제로 하는 M27(재부모화 메커니즘)만 먼저 끝나 있으면 된다.

## 완료 후

각 마일스톤(또는 몇 개씩 묶어) 완료 시 `docs/done/`에 결과를
기록한다. 이 계획 문서 자체는 실행 후에도 수정하지 않고 "실행 전
계획" 그대로 보존한다(기존 계획들과 동일한 문서 체계 원칙).
