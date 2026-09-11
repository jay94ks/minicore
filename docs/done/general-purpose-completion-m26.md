# 완료 보고: general-purpose-completion M26 — 실제 libc 포팅 재도전 (계획 최종 마일스톤)

**대상 계획**: [general-purpose-completion.md](../plan/general-purpose-completion.md) §M26
**관련 결정**: [foundations.md](../design/foundations.md) ADR-182
(musl 문자열 함수 부분집합만 실제 포팅, 전체 syscall 계층은 범위 밖)
**실행일**: 2026-09-10

## 완료한 것

### D0. 범위 좁힘 — ADR-182

M20(ADR-170)이 미뤄 둔 실제 서드파티 libc 포팅을 이번 라운드에서
다시 시도했지만, 계획 자신이 이미 "어디까지 포팅할지는 착수 시점에
다시 범위를 좁힌다"고 명시해 둔 그대로, **문자열 함수 부분집합만**
실제로 포팅했다 — 진짜 syscall 계층/동적 링커/스레드/stdio는
포함하지 않는다(ADR-170과 같은 이유: 이 커널의 근본 아키텍처가
여전히 musl이 기대하는 전제와 근본적으로 다르다). 그래서 **M26의
"셸/coreutils까지 포팅해 M20 완료 기준을 다시 달성한다"는 원래
목표는 문자 그대로 달성되지 않았다** — 셸은 여전히 minicore
네이티브 구현(ADR-170)이고, 그 안의 문자열 처리 일부가 이제 진짜
musl 소스로 이루어진다는 것만 증명한다. 자세한 근거는 ADR-182
참고.

### D1. `third_party/musl` — 이 저장소의 첫 실제 git submodule

`third_party/musl`을 v1.2.6(고정 태그)로 추가했다(`.gitmodules`,
ADR-022가 예상해 둔 형태 그대로). 무수정 원본이다 — 이번 라운드는
어떤 패치도 필요하지 않았다(`third_party/patches/musl/`는 비어
있다, `tools/apply-patches.sh`도 여전히 TODO 스텁).

### D2. `libc/` — musl 문자열 함수를 실제로 빌드

[libc/CMakeLists.txt](../../libc/CMakeLists.txt)가 musl의
`src/string/*.c` 중 syscall/스레드/락이 전혀 필요 없는 부분집합
(`memcpy`/`memmove`/`memset`/`memcmp`/`memchr`/`memrchr`/
`strcmp`/`strncmp`/`strcpy`/`stpcpy`/`strncpy`/`strcat`/`strncat`/
`strchr`/`strchrnul`/`strrchr`/`strspn`/`strcspn`/`strlen`/
`strdup`)를 무수정으로 빌드해 `minicore_libc` 정적 라이브러리를
만든다. musl 자신의 Makefile/`./configure`는 쓰지 않는다 — 이
커널의 freestanding 타깃 트리플에 맞지 않고, 이 부분집합에는
필요하지도 않다. 대신 musl이 `sed`로 만드는 `bits/alltypes.h`
생성 규칙만 새 [tools/gen-musl-alltypes.py](../../tools/gen-musl-alltypes.py)
로 재현했다(sed 출력과 개행 차이만 있고 내용은 동일함을 diff로
확인). `strdup`이 요구하는 `malloc`/`free`/`calloc`/`realloc`은
`libc/sysdeps/minicore/mem_shim.c`(이 파일 자체는 M30/ADR-204가
musl 자신의 malloc으로 대체하며 없어졌다 — 현재는
`libc/sysdeps/minicore/malloc_shim.c` 참고)가 M24(ADR-180)의
`mc_malloc`/`mc_free`로 연결한다 —
repo-layout.md가 이미 예약해 둔 "libc 내부 훅을 libmc의 mc_* 호출로
연결하는 얇은 어댑터" 자리를 이번에 처음 채웠다.

### D3. `userland/shell` — 실제 musl 함수 사용 왕복 검증

`userland/shell/main.c`가 `<string.h>`(musl의 공개 헤더)를 include해
`strcpy`+`strcat`으로 "hello"+" "+"vfs"를 조립하고, musl의
`strdup`+`strlen`+`memcmp`로 그 결과가 정확한지 확인한다
(`"[shell] libc strcpy/strcat/strdup ok=1"`) — 재구현이 아니라
실제 musl 소스가 만든 코드가 이 결과를 낸다.

### D4. 실제로 겪은 문제(빌드 시스템, 2건)

`target_include_directories`로 musl의 `arch/x86_64`/`arch/generic`/
생성된 `bits/alltypes.h`를 `minicore_libc` 자신에게만(PRIVATE)
노출했다가, musl의 `string.h`를 쓰는 소비자(`userland/shell`)가
그 헤더들이 내부적으로 요구하는 `bits/alltypes.h`, 그다음
`bits/stdint.h`를 순서대로 못 찾아 두 번 연달아 빌드가 깨졌다 —
musl의 공개 헤더를 쓰는 모든 소비자는 그 헤더가 참조하는 arch별
`bits/*.h`와 생성 헤더까지 함께 봐야 하므로, 이 셋을 PUBLIC으로
승격해 해결했다. 둘 다 컴파일 단계에서 즉시 드러났고, 고친 뒤에는
런타임 동작이 QEMU 첫 실행에 바로 맞았다(M21/M22/M25가 겪은 것과
달리 이번엔 런타임 버그가 없었다).

### D5. 검증

`tools/smoke-test-x86_64.sh`에 `"[shell] libc strcpy/strcat/strdup
ok=1"`을 추가했다. 5개 QEMU 스위트 전부 재확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0개) |

## 남겨 둔 것 (OPEN)

- **OPEN-66**: 진짜 syscall 계층(open/read/write/mmap/fork/exec을
  musl 자신의 경로로 감싸는 새 레이어)/동적 링커/pthread/locale/
  stdio(FILE/printf 계열) 포팅은 전혀 없다. 실제 포팅된 셸/coreutils
  로 M20의 완료 기준을 다시 달성하는 것은 이 OPEN이 해소된 뒤의
  일이다.

[open-items.md](../design/open-items.md)에 기록했다.

## 계획 완료

[general-purpose-completion.md](../plan/general-purpose-completion.md)
의 M21~M26 전체가 이제 완료됐다(각각 범위를 좁혀서긴 하지만, M12~M20
때와 같은 패턴 — 매 라운드 "기능적으로 검증 가능한 결과"를 실제로
남겼다). 이 계획 자체에는 더 이상 다음 마일스톤이 없다 — aarch64
이식이 사용자가 이전에 미뤄 둔 다음 방향이다(docs/index.md 참고).
