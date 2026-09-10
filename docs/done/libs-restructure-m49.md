# 완료 보고: libs-restructure M49 — `libs/` 이동 + `k`/`mc` 명명 규칙 적용

**대상 계획**: [libs-restructure.md](../plan/libs-restructure.md) §M49
**관련 결정**: [build-system.md](../design/build-system.md) ADR-199
**실행일**: 2026-09-10

## 완료한 것

1. `git mv libk libs/k`, `git mv libmc libs/mc` — 이력 보존하며 이동.
2. `git mv libs/k/include/libk libs/k/include/k` — 내부 세그먼트도
   `libk`였으므로 함께 리네임. `libs/mc/include/mc/`는 세그먼트
   이름이 이미 `mc`였으므로 그대로 뒀다(ADR-199가 예상한 그대로 —
   `#include <mc/...>` 문구는 소스에서 단 한 곳도 바뀌지 않았다).
3. 최상위 `CMakeLists.txt`의 `add_subdirectory(libk)`/
   `add_subdirectory(libmc)`를 `add_subdirectory(libs/k)`/
   `add_subdirectory(libs/mc)`로 수정.
4. 저장소 전체에서 `#include <libk/`를 `#include <k/`로 일괄
   치환했다(26개 파일 — `kernel/`, `init/initrun/`,
   `libs/k/tests/` 전부 포함). `servers/*`는 이 include를 전혀
   쓰지 않아 변경 대상이 없었다.
5. `libs/k/tests/`(구 `libk/tests/`) — 독립 CMake 프로젝트 이름과
   실행파일 이름을 `libk_tests`→`k_tests`로 통일했다(일관성을 위해
   계획에 명시되지 않았던 부분까지 추가로 정리). 이 프로젝트는
   상대경로(`../include`)만 쓰므로 디렉터리 이동 자체로 별도 경로
   수정이 필요 없었다.
6. CMake **타깃 이름**(`minicore_libk`/`minicore_libmc`)은 ADR-199
   §결정4에 따라 이번 라운드에서는 바꾸지 않았다 — 변경 범위를
   "경로+include"로만 좁혔다.

## 실제로 겪은 문제 — aarch64 빌드 실패는 이 작업과 무관

`build/aarch64-clang` 재빌드를 시도했을 때 `kernel/core/mm/phys_map.hpp`
가 요구하는 `arch_mm_defs.hpp`를 못 찾는 에러가 났다. 조사해 보니
`kernel/arch/aarch64/`에는 `CMakeLists.txt` 하나만 있고 실제 소스
파일이 전혀 없다(M3 시절부터 그랬다, `git show 8e3f1c6`로 확인) —
즉 aarch64는 이 세션의 어떤 작업과도 무관하게 원래부터 이 지점을
넘어 빌드된 적이 없다(CLAUDE.md가 이미 "aarch64는 아직 커널 코드
자체가 없다"고 명시해 둔 상태 그대로). `libs/` 이동 자체는 정상
작동했다(로그에 `-IC:/GitHub/minicore/libs/k/include`가 올바르게
찍혔다) — 이 실패를 이번 작업의 회귀로 취급하지 않는다.

## 검증

x86_64 전체 재빌드 성공(CMake 재구성이 필요해 107개 타깃 전체가
다시 빌드됨, 에러 0). bootdisk 재생성. QEMU 5개 회귀 스위트 전부
확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 88) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 11) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 24) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 12) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 6) |

동작 변화 없음(순수 이동+리네임) 확인.

## 남겨 둔 것

M50(`uapi.hpp` 폐지 + `mc` 커널/유저 통합)이 계속 진행 중.
