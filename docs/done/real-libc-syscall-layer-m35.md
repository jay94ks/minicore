# 완료 보고: real-libc-syscall-layer M35 — musl locale "C" 고정 검증

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M35
**관련 결정**: [foundations.md](../design/foundations.md) ADR-188, ADR-210
**실행일**: 2026-09-10

## 완료한 것

1. musl-hello에 locale 검증 3단계를 추가했다 — `setlocale(LC_ALL, "")`
   성공 확인, `setlocale(LC_ALL, "ko_KR.UTF-8")` 성공 확인(계획 대비
   조정, 아래 참고), `toupper('a')=='A'`/`tolower('B')=='b'`로 ctype
   동작이 여전히 C 로케일임을 확인.
2. 처음으로 필요해진 musl 소스를 컴파일에 추가했다 — `setlocale.c`,
   `locale_map.c`(`__get_locale`), `c_locale.c`(`__c_dot_utf8`/
   `__c_locale`), `__mo_lookup.c`, `getenv.c`, `toupper.c`/`tolower.c`
   /`isupper.c`/`islower.c`.
3. `locale_map.c`가 무조건 참조하는 `__map_file`(MUSL_LOCPATH 환경
   변수 경로 탐색용, 원본은 open+fstat+mmap)을 신규
   `libc/sysdeps/minicore/locale_shim.c`의 항상-실패 대체로 바꿨다 —
   이 프로젝트의 envp는 항상 비어 있어(M28) 그 경로가 실제로는
   절대 실행되지 않는다, `lock_shim.c`/`malloc_shim.c`와 같은 이유로
   fstat 등 새 의존성을 끌어올 이유가 없다.

## 계획 대비 범위 조정 (ADR-210)

계획 문서 §M35 본문은 "`setlocale(LC_ALL, "ko_KR.UTF-8")`가
실패해야 한다(NULL 반환 확인)"고 적어 뒀다. `third_party/musl/src/
locale/locale_map.c::__get_locale()`을 실제로 읽어 보니(musl 소스
무수정 원칙상 이 동작을 바꿀 수 없다) 이는 musl의 실제 동작과
다르다 — musl은 **알려지지 않은 로케일 이름을 실패시키지 않는다.**
내부 실패 센티널(`LOC_MAP_FAILED`)은 malloc 실패나 이름에 `/`·선행
`.`이 있을 때만 반환되고, 그 외엔 "요청한 이름을 기억하되 실제
데이터는 C.UTF-8로 조용히 대체"해 성공으로 처리된다 — 이것이 바로
ADR-188이 원래 겨냥한 "실제 로케일 데이터 없음"의 진짜 모습이다
(실패가 아니라 조용한 대체). 검증 목표를 조정했다: "실패해야
한다"가 아니라 "성공하지만 ctype 동작이 전혀 안 바뀐다"를 확인한다.

## 검증

x86_64 전체 재빌드 성공. QEMU에서 확인: "musl setlocale empty ok=1"
→ "musl setlocale unknown name ok=1" → "musl locale ctype still C
ok=1". QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0) |

## 남겨 둔 것

real-libc-syscall-layer.md M36(완전한 signal 계층)부터 계속
진행한다. 실제 로케일 데이터 파일, LC_* 환경변수 기반 전환, iconv
다국어 인코딩 변환은 ADR-188이 이미 명시한 대로 범위 밖이다.
