# 완료 보고: namespace-refactor M46 — `kern::arch::x86_64` 도입

**대상 계획**: [namespace-refactor.md](../plan/namespace-refactor.md) §M46
**관련 결정**: [foundations.md](../design/foundations.md) ADR-198
**실행일**: 2026-09-10

## 완료한 것

`arch_x86_64` 네임스페이스를 `kern::arch::x86_64`로 리네임했다 —
`kernel/arch/x86_64/` 전체(33개 파일, aarch64 제외). 사전 조사로
이 네임스페이스가 순수 커널 내부(`kernel/` 밖 어떤 파일도 참조하지
않음)임을 확인했으므로 M44와 같은 방식(Python 스크립트로 선언/
닫는 주석/qualified 참조 3패턴 일괄 치환)을 그대로 적용했다.

## `kern::proc` 경계 판단 — 분리하지 않기로 결정

ADR-198의 매핑표는 `kernel/arch/x86_64/process_ops.*`의 fork/exec/
spawn/kill 의미론을 `kern::proc`(arch 독립 인터페이스)으로 분리할지
이 마일스톤에서 판단하도록 남겨 뒀다. 실제로 이 코드는 레지스터
수준 컨텍스트 조작(각 아키텍처의 CPU 상태 구조체를 직접 다룸)과
"fork/exec이란 무엇인가"라는 개념이 지금 하나의 파일에 강하게
얽혀 있어, 분리하려면 그 얽힘을 실제로 풀어내는 별도 설계 작업이
필요하다 — 이번 라운드는 이름만 바꾸는 순수 리네임을 벗어나는
비용이라고 판단해 **분리하지 않는다**(namespace-refactor.md M46이
이미 "분리하지 않기로 결정해도 유효한 결과"라고 명시해 둔 대로).
`process_ops.*`는 그대로 `kern::arch::x86_64`에 남는다. `kern::proc`
자리는 만들지 않았다 — 실제로 그 경계를 분리하는 설계가 나오는
시점에 다시 판단한다.

## 검증

전체 재빌드 성공(첫 시도, 에러 0). QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0) |

동작 변화 없음(순수 리네임) 확인.

## 남겨 둔 것

- `kern::proc` 경계 분리는 여전히 미착수 — 별도 OPEN 항목으로
  등록하지 않는다(ADR-198이 이미 "판단 필요"로 표시해 둔 항목의
  연장이라 새 항목이 아니다, ADR-169와 같은 처리 방식).
- M47(`kernsrv::` 도입)~M48은 계속 진행 중.
