# M42 완료 — svcmgr 컨트롤 프로토콜 실동작

[user-service-manager.md](../plan/user-service-manager.md) §M42.
관련 ADR: [ADR-216](../design/kernel-ipc-objects.md)(kernel-ipc-objects.md),
ADR-196/197(설계, boot-and-drivers.md/registry-decisions.md, 이미
확정돼 있던 것).

## 무엇을 했는가

`servers/svcmgr`가 자기 endpoint(`k_own_endpoint_handle=1`) 위에서
`mc/svcmgr_protocol.h`의 컨트롤 프로토콜 7개 오퍼레이션
(list/status/start/stop/restart/register/unregister)을 실제로
처리하도록 만들었다.

1. **와이어 마크업 먼저(ADR-195 순서)**: `libs/mc/include/mc/
   svcmgr_protocol.h`에 `@wire-op` 한 줄 마크업(label=1~7)을 먼저
   달고 `tools/gen-wire-docs.py svcmgr libs/mc/include/mc/
   svcmgr_protocol.h`로 [docs/spec/generated/svcmgr-wire.md](../spec/generated/svcmgr-wire.md)
   를 뽑았다 — M27에 이어 이 방법론의 두 번째 적용.
2. **런타임 상태**: `runtime_unit g_runtime[8]`(used/name/running/
   thread_handle) — M41의 `@global/system/services` 레지스트리
   (선언적 유닛 목록)와 별개로, "지금 실제로 떠 있는가"를 추적하는
   svcmgr 자신만의 로컬 상태다(procsrv의 `process_entry`와 같은
   자리지만 pid가 아니라 `sys_process_kill` 대상 kernel thread
   핸들로 직접 다룬다, ADR-178/M22 패턴 재사용).
3. **오퍼레이션 7개**: `op_list`(등록된 유닛 이름 나열),
   `op_status`(running/thread_handle 조회), `op_start`/`op_stop`
   (`spawn_unit_and_wait_ready`/기존 `sys_process_kill`, ADR-178),
   `op_restart`(stop 뒤 이어서 start — NOT_RUNNING이어도 계속
   진행), `op_register`/`op_unregister`(M41의 `mc/cfgsrv_client.h`
   `set_value`/`delete_value` 재사용, ADR-196 §결정7 — 새 종료/저장
   메커니즘을 만들지 않는다는 근거 그대로).
4. **컨트롤 클라이언트**: 계획 문서가 "별도 최소 테스트 프로그램
   또는 셸의 새 빌트인, 착수 시점에 확정"이라 남겨 둔 자리를 별도
   최소 프로그램(`userland/svcmgr-ctl-test`)으로 확정했다 — 셸을
   건드리지 않고 격리해 검증할 수 있다는 게 이유다. status→stop→
   status→start→status로 M41이 부팅 시 띄운 svc-a를 왕복시키고,
   register로 svc-c를 추가한 뒤 svcmgr를 거치지 않고 cfgsrv에
   직접 물어 실제로 등록됐는지 확인한다(계획 원문의 "재부팅해
   확인"은 cfgsrv 저장 파일이 기본 memfs라 진짜 재부팅을 못 버텨
   — M41 done 참고 — 같은 부팅 안의 직접 확인으로 좁혔다).
5. **데모 유닛 변경**: `userland/svcmgr-demo-unit`이 `mc_signal_ready()`
   후 즉시 종료하던 것을 `for (;;) { mc_yield(); }`로 바꿨다 —
   op_stop이 실제로 죽일 대상이 계속 살아 있어야 검증이 성립한다.

## 실행 중 발견한 진짜 버그 3건

### 1. `init/initrun/main.cpp`의 이름→핸들 레지스트리 크기 초과

`k_max_registered_services`가 16이었는데, `svcmgr-ctl-test`를
추가하며 부팅 시 스폰되는 서비스가 18개가 됐다(svcmgr가 17번째).
17번째부터는 이름→핸들 테이블에 등록되지 못해, `svcmgr-ctl-test`의
`--depends=svcmgr-ctl-test:svcmgr,cfgsrv`에서 "svcmgr"이 조용히
빠지고 "cfgsrv"가 그 자리(handle 2)로 밀려 들어갔다 — ctl-test의
`K_SVCMGR_HANDLE=2` 관례가 실제로는 cfgsrv를 가리키게 돼, 첫
`op_status`(label=2) 호출이 cfgsrv 자신의 `op 2`(create_table)로
오해석돼 진짜 페이지폴트로 죽었다. `k_max_registered_services`를
32로 올려 해결 — 이 테이블은 부팅 시 스폰되는 서비스 전체가 쓰는
공유 자원이라 여유를 크게 뒀다.

### 2. `thread::ipc.reply_target` 단일 슬롯이 재진입 왕복을 못 견딤 (ADR-216)

가장 심각한 발견. `op_start` 처리(`handle_start`)가 아직 ctl-test의
호출에 회신하지 않은 채로 `spawn_unit_and_wait_ready()`를 불렀는데,
이 함수가 자식 프로세스의 준비완료 신호를 자식 전용 endpoint에서
`sys_recv`+`sys_reply`로 직접 받는다(ADR-193) — 같은 스레드가
스스로 클라이언트가 되는 재진입 왕복이다. 커널의
`thread::ipc.reply_target`이 스레드당 슬롯 하나뿐이라 이 안쪽
왕복이 바깥쪽(ctl-test) 회신 대상을 덮어썼고, 안쪽 `sys_reply`가
그 슬롯을 비워 바깥쪽 `sys_reply`가 "대응하는 recv 없음"(ipc.md
§3의 무동작 규칙)으로 조용히 아무 일도 하지 않는 진짜 교착이
됐다. ctl-test는 영원히 블록됐고, svcmgr는 다음 `sys_recv`에서
새 메시지를 기다리며 겉보기엔 멈춘 적이 없어 보였다 — 임시 디버그
로그로 안쪽 왕복 자체가 매번 정상 완료됨을 먼저 확인한 뒤에야
문제를 "왕복이 끝난 뒤 바깥쪽 회신이 사라지는 지점"으로 좁혔다.

M27~M41은 `spawn_unit_and_wait_ready`류 준비완료 대기를 항상 서버의
메인 IPC 루프가 시작되기 **전**(부팅 시퀀스)에서만 불렀다 — 그
시점엔 회신할 바깥쪽 호출 자체가 없어 이 경합이 드러날 수 없었다.
M42가 그 함수를 살아있는 컨트롤 호출 처리 도중 처음으로 재진입
호출한 첫 소비자였다.

**고침**: 커널에 재진입 보존 스택을 추가했다(ADR-216) —
`ipc_state`에 `reply_target_saved[4]`+`reply_target_saved_count`를
추가하고, `kernel/core/ipc/endpoint.cpp`에 `push_reply_target`/
`pop_reply_target` 헬퍼를 신설해 `sys_call`/`sys_recv`의 두
"회신 대상 확정" 지점에서 이전 값을 스택에 보존하고, `sys_reply`가
회신을 마친 뒤 그 값을 되돌리도록 바꿨다. `docs/spec/ipc.md` §3.2
로 스펙에도 반영했다.

### 2b. `handle_register`가 이미 해제된 IPC 매핑을 다시 읽음

위 수정 후 `op_start`는 통과했지만 `op_register`에서 새로운
페이지폴트가 드러났다. `handle_register`가 수신 메시지의 IPC 매핑
슬롯(`in.pages[0].vaddr`)을 가리키는 원시 포인터(`unit`)를 nested
`mc_reg_set_binary()` 호출(cfgsrv에게 등록을 요청) **뒤까지** 들고
있다가 `alloc_runtime(unit->name)`에서 다시 읽었다 — 그 nested
호출이 cfgsrv의 응답을 받는 순간 이 스레드가 다시
`deliver_message`의 목적지가 되어(기존 ADR-161의
`release_previous_ipc_mapping`) 매핑이 이미 해제된 뒤였다. 이건
커널 설계의 간극이 아니라 ADR-161이 이미 명시한 규칙("이 스레드가
다시 배달 목적지가 되는 시점에 이전 매핑을 해제한다")을 svcmgr
코드가 어긴 것이다 — `mc/cfgsrv_client.c`가 이미 지키고 있는
관례(nested 호출 전에 값을 자기 정적 버퍼로 복사)를 그대로 따라,
수신 즉시 `mc_svcmgr_service_unit` 구조체 전체를 로컬 변수로
복사하도록 고쳤다. 새 ADR은 필요하지 않다(ADR-216 안에 이 발견도
함께 기록).

## 검증 (QEMU, x86_64)

`tools/smoke-test-x86_64.sh`에 M42 어서션 7개 추가, 전체 스모크
스위트(기존 어서션 전부 포함) PASS(exit 0). 추가로 SMP/NUMA/AVX/
net 4개 회귀 스위트 전부 PASS(exit 0, FAIL 없음) — 커널의
`kernel/core/ipc/endpoint.cpp`/`kernel/core/object/kernel_objects.hpp`
변경이 다른 IPC 소비자(procsrv/vfs/fat32/ext4/netsrv/cfgsrv/login/
shell 등)에 회귀를 만들지 않았음을 확인했다.

핵심 확인 로그:

```
[svcmgr-ctl-test] status svc-a running=1
[svcmgr-ctl-test] stop svc-a ok=1
[svcmgr-ctl-test] status svc-a stopped=1
[svcmgr-ctl-test] start svc-a ok=1
[svcmgr-ctl-test] status svc-a running again=1
[svcmgr-ctl-test] register svc-c ok=1
[svcmgr-ctl-test] cfgsrv sees svc-c ok=1
```

## 범위 밖으로 남긴 것

- `exec_path`(VFS 경로)는 여전히 안 읽는다 — 등록된 유닛이 몇 개든
  전부 같은 임베딩된 데모 ELF를 실행한다(M41부터 이어지는 제약,
  계획 범위 밖).
- fs-protocol의 close 부재(OPEN-70, M41이 발견)는 이번에도 손대지
  않았다.
- `ipc_state::reply_target_saved`의 깊이 4는 지금 실제로 필요한
  깊이(1)보다 넉넉한 임의의 여유일 뿐, 더 깊은 재진입이 필요한
  소비자가 나오면 그때 다시 검토한다(YAGNI).

## 다음

M43(계정별 유저 서비스 인스턴스, 계획 문서 자신이 스트레치로
표시 — "이 마일스톤이 착수되지 않거나 실패해도 M40~M42의 성과는
독립적으로 유효하다").
