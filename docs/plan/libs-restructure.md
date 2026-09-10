# 실행 계획: 라이브러리 저장소 재배치 + `mc` 커널/유저 통합 (M49~M50)

**관련 결정**: [build-system.md](../design/build-system.md) ADR-199
(`libs/` 이동 + `lib` 접두사 제거 명명 규칙), [foundations.md](../design/foundations.md)
ADR-200(`mc`를 커널·유저 공용으로 통합, `uapi.hpp` 폐지, ADR-132 보강),
[repo-layout.md](../design/repo-layout.md)(목표 트리 이미 반영)

**선행 완료 전제**: 없음 — 다른 계획의 완료를 전제하지 않는다. 다만
[namespace-refactor.md](namespace-refactor.md) M45("`uapi`→`kern::proto`
리네임")는 이 계획의 M50으로 **대체**됐다 — 두 계획을 동시에 진행할
경우 M45는 건드리지 않고 이 계획의 M50만 실행한다(중복 작업 방지).
namespace-refactor.md의 나머지 마일스톤(M44/M46/M47/M48)은 이 계획과
독립적이다.

## 배경

ADR-199/200이 두 가지를 정했다 — (1) 이 저장소가 직접 만들고 유지·
관리하는 라이브러리(`libk`, `libmc`)를 `libs/` 하위로 옮기고 `lib`
접두사를 뗀 이름(`k`, `mc`)을 쓴다, (2) `mc`(구 `libmc`)가 커널과
유저랜드 양쪽의 공용 프로토콜/ABI 출처가 되어, 지금 `kernel/include/
uapi.hpp`가 손으로 복제해 온 커널 syscall ABI를 흡수한다. 둘 다
규칙만 확정됐고 실제 코드 변경은 없다 — 이 계획이 그 실행을 다룬다.

## M49. `libs/` 이동 + `k`/`mc` 명명 규칙 적용 (순수 이동/리네임)

- **구현**: [ADR-199](../design/build-system.md)를 실제로 적용한다.
  1. `libk/` → `libs/k/`, `libmc/` → `libs/mc/`로 디렉터리를
     옮긴다(`git mv`로 이력 보존).
  2. **인클루드 경로**: `libs/mc/include/mc/`는 세그먼트 이름이
     이미 `mc`였으므로 `#include <mc/...>` 문구는 소스에서 바뀌지
     않는다 — CMake `target_include_directories`만 새 경로로
     수정한다. `libs/k/include/libk/`는 `libs/k/include/k/`로
     바꿔야 하므로, `#include <libk/...>`를 쓰는 **모든 소비자**
     (`kernel/`, `servers/*` 전체)를 `#include <k/...>`로 일괄
     치환한다.
  3. 최상위 `CMakeLists.txt`/각 `CMakeLists.txt`의 `add_subdirectory(libk)`/
     `add_subdirectory(libmc)` 경로를 `libs/k`/`libs/mc`로 수정한다.
     CMake 타깃 이름(`minicore_libk`/`minicore_libmc`)을 바꿀지는
     이 마일스톤 착수 시점에 결정한다(ADR-199 §결정4).
- **목표**: 전체 재빌드 성공 + 5개 QEMU 스위트(smoke/SMP/NUMA/AVX/net)
  전부 이동 전과 동일하게 통과(순수 이동이라 동작 변화 없음).

## M50. `uapi.hpp` 폐지 — `mc`로 흡수 + 커널-랜드/유저-랜드 매크로 도입

- **구현**: [ADR-200](../design/foundations.md)을 실제로 적용한다.
  1. `kernel/include/uapi.hpp`의 내용(message 레이아웃, syscall
     번호, 각 요청/응답 구조체)을 `libs/mc/include/mc/`의 적절한
     헤더(신규 또는 기존 확장, 착수 시점에 파일 구성 확정)로
     옮긴다.
  2. 그 헤더에 커널-랜드 매크로 가드를 추가한다(가칭
     `MC_LAND_KERNEL` — 정확한 이름 확정) — 매크로가 정의돼 있으면
     구조체/enum/상수만 노출하고 syscall 트램폴린/프로토콜 클라이언트
     함수 선언은 숨긴다. 매크로가 없으면(기존 유저랜드 소비자)
     지금과 동일하게 전부 노출한다.
  3. `kernel/arch/x86_64/{syscall.cpp,process_ops.*,kernel_main.cpp}`
     가 `MC_LAND_KERNEL`을 정의하고 `mc`의 헤더를 include하도록
     바꾸고, 기존 `uapi::` 참조를 새 네임스페이스 참조로 바꾼다
     (정확한 네임스페이스는 [namespace-refactor.md](namespace-refactor.md)
     M44/M46 진행 상황에 맞춘다 — 이 계획과 순서를 조율해야 한다).
  4. `kernel/include/uapi.hpp`를 삭제한다.
  5. `servers/*`(전부)와 `init/initrun/main.cpp`는 이미 유저-랜드
     소비자이므로 매크로 없이 그대로 `mc`의 헤더를 참조한다 —
     `uapi::` 참조를 `mc::`(또는 확정된 네임스페이스)로 치환한다.
- **목표**: 전체 재빌드 성공(커널이 처음으로 `mc`의 헤더를 참조하게
  됨을 확인) + 5개 QEMU 스위트 전부 회귀 없음. `kernel/include/uapi.hpp`
  가 실제로 삭제됐고 커널·유저 양쪽이 같은 헤더의 같은 구조체
  정의를 참조함을 확인한다(더 이상 손으로 맞춘 두 개의 사본이
  아니다).

## 포함하지 않는 것

- CMake 타깃 이름 변경 여부, 매크로의 정확한 이름 — 각 마일스톤
  착수 시점에 확정(ADR-199/200이 이미 명시).
- `libk_detail`→`libk::__internals__`(또는 `k::__internals__`)
  여부 — ADR-198이 이미 별도 결정 대상으로 남겨 뒀다. 이 계획에서
  다루지 않는다.
- `mc`의 구현(.c) 파일을 커널이 링크하는 것 — ADR-200 §결정4가
  이미 범위 밖으로 명시(헤더만 공유).

## 검증 방법

M49/M50 둘 다 동작 변화가 없어야 하는 순수 리팩터다 —
[namespace-refactor.md](namespace-refactor.md)와 같은 방식으로,
새 QEMU 확인 문자열이 아니라 **기존 5개 스위트가 리팩터 전과
동일하게 통과하는지**만 확인한다.

## 완료 후

각 마일스톤 완료 시 `docs/done/`에 결과를 기록한다. 이 계획 문서
자체는 실행 후에도 수정하지 않고 "실행 전 계획" 그대로 보존한다.
