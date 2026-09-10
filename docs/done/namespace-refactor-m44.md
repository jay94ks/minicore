# 완료 보고: namespace-refactor M44 — `kern::` 최상위 도입, 커널 코어 서브시스템 리네임

**대상 계획**: [namespace-refactor.md](../plan/namespace-refactor.md) §M44
**관련 결정**: [foundations.md](../design/foundations.md) ADR-198(규칙),
M44 실행 결과 절(실제 겪은 문제 포함)
**실행일**: 2026-09-10

## 완료한 것

`kernel/core/*`와 그 확장(`kernel/arch/x86_64/`가 같은 네임스페이스를
여는 파일들, `kernel/include/klog.hpp`)의 6개 네임스페이스를
`kern::*`로 리네임했다:

| 기존 | 신규 |
|---|---|
| `object` | `kern::object` |
| `ipc` | `kern::ipc` |
| `mm` | `kern::mm` |
| `sched` | `kern::sched` |
| `klog` | `kern::klog` |
| `initrd` | `kern::initrd` |

Python 스크립트로 `kernel/**/*.hpp`/`kernel/**/*.cpp`(aarch64 제외)
전체를 훑어 세 가지 패턴을 일괄 치환했다 — `namespace X {` 선언,
`}  // namespace X` 닫는 주석, 그리고 단어 경계 기준 `X::` 참조. 총
48개 파일이 바뀌었다(`kernel/include/uapi.hpp`도 포함 — 실제 코드는
바뀌지 않고 `ipc::message`/`object::k_right_can_*`/`sched::exit()`
등을 언급하는 **주석**만 새 이름으로 갱신됐다, `uapi.hpp` 자체는
M50에서 폐지될 예정이라 임시로만 정확해진 상태다).

## 실제로 겪은 문제 — `boot` 네임스페이스의 범위 재조정

계획(ADR-198의 매핑표)은 `boot`(`kernel/include/boot_info.hpp`,
`kernel/core/boot_info_dump.cpp`)도 `kern::boot`로 리네임할
대상으로 표시해 뒀었다. 실행 착수 시 실제 참조를 조사해 보니
`init/initrun/main.cpp`(유저랜드!)가 `boot_info.hpp`를 **직접
include**해 `boot::boot_info`를 그대로 쓰고 있었다 — 이 헤더가
`kernel/include/`(repo-layout.md가 "유저랜드와 공유하는 커널 ABI
헤더" 자리로 정의한 디렉터리)에 있는 이유가 정확히 이것이었다.
즉 `boot`는 `uapi`와 같은 "커널·유저 공유 ABI" 범주이지, `kern::`
(커널 전용) 대상이 아니다.

**대응**: `boot`를 이번 리네임에서 제외했다(이름 그대로 유지) —
ADR-198의 매핑표를 이 발견에 맞춰 갱신하고, `initrd`의 자리도
"`kern::boot` 하위"에서 "`kern::initrd`(최상위)"로 확정했다
(더 이상 상위로 삼을 `kern::boot`가 없으므로). `boot`의 장기적
처리(ADR-200이 `uapi.hpp`에 이미 적용한 것처럼 `mc`로 흡수하는
것이 방향상 맞다)는 이 마일스톤의 범위를 넓히지 않고 후속 결정
대상으로 남겼다.

## 검증

전체 재빌드(`cmake --build build/x86_64-clang -j 8`) 첫 시도에
성공(에러 0, 무관한 기존 경고 1개만). QEMU 5개 회귀 스위트 전부
확인(이 환경은 `qemu-system-x86_64`가 PATH에 없어
`MINICORE_QEMU_BIN` 환경변수로 실제 설치 경로를 지정해 실행함,
`C:\Program Files\qemu\qemu-system-x86_64.exe`):

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 11) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 24) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 12) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 6) |

동작 변화 없음(순수 리네임) 확인.

## 남겨 둔 것

- `boot` 네임스페이스의 장기 처리(위 "실제로 겪은 문제" 참고) —
  새 OPEN 항목으로 등록하지 않고, ADR-198/200의 연장선 후속
  결정으로만 기록한다.
- `namespace-refactor.md`의 나머지 마일스톤: M45는
  [libs-restructure.md](../plan/libs-restructure.md) M50으로 대체됨(스킵),
  M46(`kern::arch::x86_64`+`kern::proc` 경계)~M48(`kernsrv::proto`)
  은 아직 착수 전.
