# 완료 보고: real-libc-syscall-layer M30 — TLS(M28로 이미 흡수) + 실제 musl 힙 할당자

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M30
**관련 결정**: [kernel-memory.md](../design/kernel-memory.md) ADR-183, ADR-204
**실행일**: 2026-09-10

## 완료한 것

1. **TLS(`arch_prctl`) 부분은 이미 M28에서 완료됐다**(ADR-202) —
   M30 착수 시점에는 계획이 원래 예정했던 두 항목 중
   `SYS_mmap`/`SYS_munmap`(musl 자신의 malloc)만 남아 있었다.
2. 새 커널 syscall `MC_SYSCALL_MMAP_ANON`/`MC_SYSCALL_MUNMAP` —
   `kern::arch::x86_64::mmap_anon()`/`munmap_anon()`
   (`process_ops.cpp`)이 `sys_brk`와 **완전히 분리된** 새 영역
   (`kern::object::address_space::mmap_top`, ADR-160 슬롯 6, 4MiB
   예산)에서 익명 페이지를 매핑한다. `munmap`은 M24의 `sys_brk`
   축소 미지원과 같은 정신으로 실제 회수를 하지 않는다(항상 성공).
3. `libmc` 트램폴린 `mc_mmap_anon()`/`mc_munmap()`(`mc/syscall.h`).
4. `libc/sysdeps/minicore/syscall_shim.c` — `SYS_mmap`(익명만,
   `mc_mmap_anon` 호출)/`SYS_munmap`(`mc_munmap` 호출) 구현. `SYS_brk`
   는 **의도적으로 항상 실패**(0)를 반환한다 — 아래 "실행 전 발견"
   참고.
5. `libc/CMakeLists.txt` — M26의 `mem_shim.c`(손으로 짠
   `malloc/free/calloc/realloc` 어댑터)를 제거하고, musl 자신의
   `src/malloc/lite_malloc.c`(순수 범프 할당자, mallocng보다 단순 —
   아래 참고)+`calloc.c`+`free.c`+`replaced.c`+`src/mman/{mmap,munmap}.c`
   로 대체했다. `libc/sysdeps/minicore/lock_shim.c`(신규, musl
   내부 `__lock`/`__unlock`을 단일 스레드 전용 no-op으로 대체 —
   원본은 futex 기반이라 pthread 없는 지금은 불필요)와
   `malloc_shim.c`(신규, `__libc_free` no-op — `lite_malloc.c`는
   free를 지원하지 않는다, M24/M26과 같은 "알려진 단순화")를
   추가했다.
6. `userland/musl-hello/main.c` 확장 — 진짜 musl `malloc(64)`+
   `memcpy`+`memcmp`+`free()` 왕복("musl malloc ok=1"/"musl malloc
   content ok=1")과, 잘못된 fd로 `write()`를 호출해 `errno`가 실제로
   `EBADF`로 설정됨을 확인("musl errno ok=1" — M28의 FS_BASE/TLS
   경로가 이미 갖춰 둔 것을 실제로 행사하는 첫 사례).

## 실행 전 발견 — musl의 malloc과 libmc의 mc_malloc이 같은 sys_brk 상태를 공유하면 충돌

[ADR-204](../design/kernel-memory.md)에 전체 판단을 기록했다. 요약:
musl의 기본 할당자 `mallocng`은 `mmap`+`mprotect`까지 요구하는
정교한 설계라 이번 라운드 범위를 넘어선다고 판단해 더 단순한
`lite_malloc.c`(musl이 실제로 제공하는 빌드 옵션)를 골랐는데, 이
할당자는 **항상 먼저 `SYS_brk`로 확장을 시도**한다. 코드를 분석해
보니 `libmc`의 `mc_malloc`(ADR-180, 셸이 직접 호출)이 이미 같은
`sys_brk` 커널 상태(`address_space::heap_top`)를 쓰고 있고, 최초
호출 시 캐싱한 `g_heap_cursor`를 이후 `sys_brk` 확장 시에도
재동기화하지 않는다는 것을 발견했다 — musl의 malloc도 같은 커널
상태를 건드리게 되면 두 할당자가 서로 모르게 겹치는 영역을 "자기
것"으로 믿을 수 있다(메모리 손상 위험). **실행해 보고 겪은 문제가
아니라 실행 전 분석으로 미리 잡아낸 문제**라는 점이 M28/M29의 "실행
중 발견"과 다르다. 해결: `syscall_shim.c`의 `SYS_brk`를 항상
실패시켜(musl 자신의 기존 mmap 폴백 로직을 그대로 이용, musl 소스는
무수정) musl의 malloc이 무조건 완전히 분리된 새 `SYS_mmap` 영역만
쓰게 만들었다 — 셸의 기존 `mc_malloc()` 직접 호출도 musl 소스도
건드리지 않았다.

## 검증

x86_64 전체 재빌드 성공. bootdisk 재생성. QEMU에서 확인:
`hello from real musl` → `musl malloc ok=1` → `musl malloc content
ok=1` → `musl free done` → `musl errno ok=1`. 이후 나머지 부팅
(initrun/procsrv/셸 등, `mem_shim.c` 제거의 영향을 받을 수 있는
셸의 `strdup`/`mc_malloc` 자기테스트 포함)이 평소와 동일하게 계속됨
(회귀 없음). QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0) — 신규 확인 문자열(musl malloc/content/errno) 포함 |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0) |

## 남겨 둔 것

real-libc-syscall-layer.md M31(파일 I/O syscall — VFS/FS IPC 연결 +
실제 musl stdio)부터 계속 진행한다. ADR-203(M29)에 따라 정적 링킹
기반으로 계속된다.
