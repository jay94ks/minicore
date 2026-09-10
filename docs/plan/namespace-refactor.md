# 실행 계획: 네임스페이스 컨벤션 적용 (`kern::*`/`kernsrv::*`) (M44~M48)

**관련 결정**: [foundations.md](../design/foundations.md) ADR-198
(네임스페이스 컨벤션 규칙 확정, 이 계획이 그 실제 적용을 다룬다),
[cxx-conventions.md](../spec/cxx-conventions.md) §6(이미 목표 상태로
갱신됨)

**선행 완료 전제**: 없음 — 이 계획은 다른 어떤 계획의 완료도 전제하지
않는다(순수 리네임이라 기능적 의존관계가 없다). 다만 동시에 진행
중인 다른 계획([real-libc-syscall-layer.md](real-libc-syscall-layer.md),
[user-service-manager.md](user-service-manager.md))이 건드리는 파일과
겹칠 수 있으므로, **이 계획의 각 마일스톤은 그 시점에 다른 계획이
진행 중이지 않은 파일부터 순서를 잡거나, merge 충돌을 감수하고
나중에 리베이스한다** — 착수 시점에 실제 상황에 맞게 조정한다.

## 배경

ADR-198이 네임스페이스 계층 규칙(`kern::*`, `kernsrv::<서버명>`,
`__internals__` 은닉, `proto` 예외)을 확정했지만 규칙만 정했을 뿐 —
기존 `kernel/`·`servers/*`의 실제 코드는 여전히 `object`/`ipc`/`mm`/
`sched`/`arch_x86_64`/`uapi`(전부 평면 네임스페이스)와 서버들의
무네임스페이스 상태 그대로다. 이 계획이 그 리네임을 실행한다.

**성격이 다른 작업임을 명시한다**: 이 계획의 모든 마일스톤은 순수
기계적 리네임(동작 변화 없음)이다 — 새 기능이나 버그 수정이 아니다.
그래서 각 마일스톤의 "목표"는 새 QEMU 확인 문자열을 추가하는 게
아니라, **기존 스모크/SMP/NUMA/AVX/net 5개 스위트가 전부 리네임 전과
동일하게 통과하는 것**(회귀 없음)이다. 컴파일러가 놓친 참조를 즉시
잡아준다는 점(잘못된 네임스페이스 참조는 빌드 실패로 드러난다)이
이 종류의 작업의 안전판이다.

## M44. `kern::` 최상위 도입 — 커널 코어(`kernel/core/*`) 서브시스템 리네임

- **구현**: [ADR-198](../design/foundations.md) 매핑표의 커널 코어
  부분을 적용한다 — `object`→`kern::object`, `ipc`→`kern::ipc`,
  `mm`→`kern::mm`, `sched`→`kern::sched`, `boot`→`kern::boot`,
  `klog`→`kern::klog`. **`initrd`의 정확한 자리(`kern::initrd` 최상위
  vs `kern::boot::initrd` 하위)는 이 마일스톤 착수 시점에 확정한다**
  (ADR-198이 판단을 미뤄 둔 지점). `kernel/arch/x86_64/`가 `object`
  네임스페이스에 추가하는 부분(`fpu.hpp`/`process_ops.hpp`/`tss.hpp`)
  도 함께 `kern::object`로 옮긴다 — `arch_x86_64` 자체(M46)는 이
  마일스톤 범위 밖.
- **목표**: 전체 재빌드 성공 + `smoke`/`smp`/`numa`/`avx` 4개 QEMU
  스위트가 리네임 전과 동일한 문자열로 통과(net 스위트는 이 시점엔
  아직 `uapi`/`arch_x86_64`에 걸려 있어 M45/M46 이후 확인).

## M45. `kern::proto` 도입 — `uapi` 리네임 (**대체됨, 아래 참고**)

- **상태: 이 마일스톤은 [foundations.md](../design/foundations.md)
  ADR-200으로 대체됐다** — `uapi`를 `kern::proto`로 단순
  리네임하는 대신, `uapi.hpp` 자체를 폐지하고 `mc`(구 `libmc`)의
  헤더로 흡수하기로 방향이 바뀌었다(전처리기 매크로로 커널-랜드/
  유저-랜드를 가른다). 실제 실행은 이 문서가 아니라
  [libs-restructure.md](libs-restructure.md) M50이 담당한다 — 이
  문서 자체는 수정하지 않는다는 원칙에 따라 원안을 아래에 그대로
  남겨 둔다(참고용, 실행하지 않음).
- **(원안, 실행 안 함) 구현**: `kernel/include/uapi.hpp`의
  `namespace uapi`를 `namespace kern::proto`로 바꾼다. 이 참조는
  `kernel/`뿐 아니라 `init/initrun/main.cpp`와 `servers/*` 14개
  파일 전부에 퍼져 있다(사전 조사로 확인함) — 전체 저장소에서
  `uapi::`를 `kern::proto::`로 일괄 치환하는 순수 리네임이었다.
- **(원안) 목표**: 전체 재빌드 성공(커널+`init/initrun`+`servers/*`
  전부) + 5개 QEMU 스위트(smoke/SMP/NUMA/AVX/net) 전부 리네임 전과
  동일하게 통과.

## M46. `kern::arch::x86_64` 도입 + `kern::proc` 경계 정리

- **구현**: `arch_x86_64` 네임스페이스를 `kern::arch::x86_64`로
  바꾼다(M44/M45과 달리 이 마일스톤은 순수 치환에 실제 설계 판단이
  하나 섞인다) — **`kernel/arch/x86_64/process_ops.*`의 fork/exec/
  spawn/kill 의미론을 `kern::proc`(ADR-198이 새로 연 자리)으로
  분리할지, 그대로 `kern::arch::x86_64`에 둘지를 이 시점에 확정한다.**
  ADR-002(HAL 경계)의 정신대로 분리하는 쪽을 권장하되(레지스터 수준
  구현은 `kern::arch::x86_64`, "fork란 무엇인가"라는 arch 독립
  개념은 `kern::proc`), 실제로 분리하면 지금 arch_x86_64 하나에
  섞여 있는 코드를 두 파일/네임스페이스로 쪼개는 추가 작업이 필요해
  이 마일스톤의 범위가 커진다 — 착수 시점에 비용 대비 가치를
  판단한다(분리하지 않기로 결정해도 유효한 결과다, ADR-049과 같은
  기준).
- **목표**: 전체 재빌드 성공 + 5개 QEMU 스위트 전부 회귀 없음.

## M47. `kernsrv::` 도입 — `servers/*` 전체를 네임스페이스로 감싸기

- **구현**: 서버마다(`procsrv`/`vfs`/`devmgr`/`cfgsrv`/`login`/
  `netsrv`/`fs::memfs`/`fs::fat32`/`fs::ext4`/`drivers::ps2`/
  `drivers::usb`/`drivers::console`/`drivers::virtio_blk`/
  `drivers::virtio_net`) 구현 본체를 `namespace kernsrv::<이름> { ... }`
  로 감싼다. **엔트리 포인트 심볼(각 서버의 실제 시작 함수 — 정확한
  이름/관례는 M12/ADR-131이 정한 argv/boot_info 규약을 착수 시점에
  다시 확인)은 전역(또는 `extern "C"`) 그대로 둬야 한다** — 네임스페이스로
  감싸면 링커/런타임이 찾지 못한다. 지금까지 서버들은 파일 내부
  익명 네임스페이스만 썼으므로(무네임스페이스 상태), 이 마일스톤은
  M44~M46과 달리 "참조를 바꾸는" 리네임이 아니라 "새로 감싸는"
  추가 작업이다 — 서버는 서로 IPC로만 통신하므로(직접 링크되는
  cross-server 참조가 없다) 각 서버는 독립적으로 안전하게 처리할
  수 있다.
- **목표**: 전체 재빌드 성공 + 5개 QEMU 스위트 전부 회귀 없음(모든
  서버가 이전과 동일하게 기동·통신함을 확인).

## M48. `kernsrv::proto` 분리 — 서버 내부 순수 C++ 프로토콜 코드 이전

- **구현**: `libmc`가 정본이 아닌, 서버 내부에만 있는 C++ 프로토콜/
  와이어 포맷 코드(예: `netsrv`의 이더넷/IP/UDP 헤더 구조체, M25)를
  `kernsrv::<서버명>`에서 `kernsrv::proto`로 옮긴다. 착수 시점에
  실제로 이 범주에 해당하는 코드가 무엇인지 다시 확인한다(사전
  조사 기준으로는 `netsrv`가 유일한 후보).
- **목표**: 전체 재빌드 성공 + 5개 QEMU 스위트 전부 회귀 없음.

## 포함하지 않는 것

- **`__internals__` 마커의 소급 적용** — ADR-198 §결정3이 이미
  "강제하지 않음"으로 정해 뒀다. 기존 익명 네임스페이스를
  `__internals__`로 바꾸는 작업은 이 계획에 없다 — 실제로 한 모듈의
  여러 파일이 공유해야 하는 헬퍼가 생기는 시점에 그때그때 도입한다.
- **`libk`의 `libk_detail`→`libk::__internals__` 여부** — ADR-198이
  이미 별도 결정 대상으로 남겨 뒀다. 이 계획에서 다루지 않는다.
- **`libmc`/`libc`/`userland`** — ADR-198 §결정5가 이미 적용 대상이
  아니라고 명시했다.

## 검증 방법

각 마일스톤은 동작 변화가 없어야 하므로, 검증은 "새 문자열 추가"가
아니라 **기존 5개 스위트(`tools/smoke-test-x86_64.sh`,
`smoke-test-smp-x86_64.sh`, `smoke-test-numa-x86_64.sh`,
`smoke-test-avx-x86_64.sh`, `smoke-test-net-x86_64.sh`)가 리네임
전과 정확히 동일한 결과를 낸다"는 것만 확인한다. 빌드 실패(놓친
참조)는 컴파일러가 즉시 알려주므로, 그 자체가 1차 검증이다.

## 완료 후

각 마일스톤 완료 시 `docs/done/`에 결과를 기록한다(무엇을 리네임했고
회귀가 없었는지만 간단히). 전부 끝나면 [cxx-conventions.md](../spec/cxx-conventions.md)
§6의 "기존 코드 리네임은 아직 실행되지 않았다" 문구를 지운다. 이
계획 문서 자체는 실행 후에도 수정하지 않고 "실행 전 계획" 그대로
보존한다(기존 계획들과 동일한 문서 체계 원칙).
