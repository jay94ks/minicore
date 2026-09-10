# 완료 보고: real-libc-syscall-layer M28 — musl syscall 번역 계층 착수, 최초의 실제 musl 프로그램(정적)

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M28
**관련 결정**: [foundations.md](../design/foundations.md) ADR-183,
[kernel-memory.md](../design/kernel-memory.md) ADR-202
**실행일**: 2026-09-10

## 완료한 것

1. `tools/apply-patches.sh`(신규 구현, 이전엔 TODO 스텁) —
   `third_party/musl` 체크아웃을 `<output-dir>`로 복사하고(제출된
   서브모듈 자체는 pristine 유지) `third_party/patches/musl/*.patch`
   를 이름순으로 표준 `patch(1)` 유틸리티로 적용한다. `git apply`가
   아니라 `patch(1)`을 쓴다 — 이 저장소의 `core.autocrlf=true`
   설정이 `git apply`의 CRLF 정규화와 충돌해 "Skipped patch"로
   조용히 아무것도 안 하는 것을 실제로 겪었다(아래 "겪은 문제"
   참고).
2. `third_party/patches/musl/0001-syscall-shim.patch`(신규) —
   `arch/x86_64/syscall_arch.h`의 `__syscall0`~`__syscall6`이 raw
   `syscall` x86_64 명령 대신 `__minicore_syscall_dispatch`(신규
   `libc/sysdeps/minicore/syscall_shim.c`)를 호출하게 재작성한다.
3. `libc/sysdeps/minicore/syscall_shim.c`(신규) — `SYS_write`(fd
   1/2만, `mc_debug_log` 호출), `SYS_exit`/`SYS_exit_group`
   (`mc_thread_exit` 호출), `SYS_arch_prctl`(`ARCH_SET_FS`만,
   `mc_arch_prctl_set_fs` 호출)을 구현한다. 그 외 번호는 항상
   `-ENOSYS`를 반환하고 `[syscall_shim] unimplemented n=<번호>`를
   `debug_log`로 남긴다(계획의 "검증 방법" 요구사항).
4. `libc/CMakeLists.txt` 확장 — musl의 진짜 시작 경로(`crt/crt1.c`,
   `src/env/__libc_start_main.c`, `src/env/__init_tls.c` 등 12개
   파일)를 `minicore_libc`에 추가했다. 이 파일들은 원본이 아니라
   `tools/apply-patches.sh`가 만든 패치된 트리(`_patched/musl`,
   빌드 디렉터리 안)에서 가져온다 — 지금은 헤더 하나만 실제로
   다르지만 "musl 소스는 항상 패치 파이프라인을 통과한 것을 쓴다"는
   불변식을 지킨다. `tools/gen-musl-syscall-bits.py`(신규, musl
   Makefile의 `bits/syscall.h` 생성 규칙을 파이썬으로 재구현 —
   `gen-musl-alltypes.py`와 같은 이유)도 추가했다.
5. `userland/musl-hello/`(신규) — musl로 실제로 링크된 최초의
   유저 프로그램. `main.c`가 `write(1, "hello from real musl\n",
   ...)`+`_exit(0)`을 호출한다(둘 다 musl 자신의 라이브러리 함수,
   syscall 매크로 직접 호출이 아니다). `link.ld`는 initrun과 같은
   베이스 주소를 쓰되 `.init_array`/`.fini_array` 섹션을 추가로
   정의한다(`__libc_start_main.c`가 그 경계 심볼을 참조한다).
6. `kernel_main.cpp::spawn_preempt_demo_processes()`에 musl-hello
   스폰을 추가했다(M21 preempt 데모와 같은 자리 — `init/initrun/
   CMakeLists.txt`가 initrd에 `musl_hello=...`로 함께 담는다).
   `linux_abi_stack=true`로 스폰하는 것이 기존 두 데모와의 유일한
   차이다.

## 실제로 겪은 문제 (계획을 상당히 앞당긴 두 가지 발견)

M28의 원래 계획 텍스트는 "이 마일스톤은 `SYS_write`/`SYS_exit`/
`SYS_exit_group`만 구현하고, `arch_prctl`(TLS)은 M30에 미룬다"고
적어 뒀다. 실제로 musl의 진짜 시작 경로를 추적해 보니 이 경계가
musl 자신의 초기화 순서와 맞지 않았다 — 두 가지를 실제로 앞당겨야만
`main()`에 도달할 수 있었다([ADR-202](../design/kernel-memory.md)에
전체 근거를 기록했다):

1. **`arch_prctl(ARCH_SET_FS)`가 M28에도 무조건 필요하다** —
   `__init_libc`→`__init_tls`→`__init_tp`→`__set_thread_area`가
   TLS 모듈이 하나도 없는 경우에도 항상 이 syscall을 호출하고,
   실패하면(`-ENOSYS`) `a_crash()`로 **`main()` 도달 전에 즉시
   죽는다**. 커널에 실제 FS_BASE MSR 지원을 추가했다 —
   `kern::object::thread::fs_base`(신규 필드) + 컨텍스트 스위치
   4곳에서 `arch_sync_io_permission`/`arch_sync_exception_stack`과
   함께 부르는 `kern::arch::x86_64::sync_fs_base`(신규,
   `kernel/arch/x86_64/tss.cpp`) + 새 syscall
   `MC_SYSCALL_ARCH_PRCTL_SET_FS`(14번). musl 쪽은
   `libc/sysdeps/minicore/set_thread_area.c`(신규)로 연결했다 —
   원본 `third_party/musl/src/thread/x86_64/__set_thread_area.s`는
   raw `syscall` 명령을 손으로 직접 발행해 패치된 `syscall_arch.h`의
   우회 경로를 완전히 건너뛰므로 이 파일은 링크 대상에서 뺐다.
2. **커널이 유저 스택에 아무것도 초기화해 두지 않는다** — musl의
   `crt_arch.h`(`_start`)는 `%rsp`를 그대로 읽어 `argc`부터
   해석하는데, 이 커널의 `build_process()`는 스택 페이지를 매핑만
   하고 내용은 미초기화(이전 물리 페이지의 쓰레기 값)로 남겨 둔다
   — 그대로 musl을 스폰하면 쓰레기 값을 포인터로 역참조하다 죽는다.
   `mc_process_spawn_request`에 `linux_abi_stack`(bool) 필드를
   추가해, true일 때만 `build_process()`가 유저 스택 최상단에
   최소 Linux ABI 스택(`argc=1`, `argv=["/bin/musl-hello"]`,
   `envp=[]`, `AT_PAGESZ`/`AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID`/
   `AT_SECURE`/`AT_PHDR`/`AT_PHNUM`/`AT_NULL`만 채운 auxv)을 직접
   써 넣는다. `AT_UID==AT_EUID && AT_GID==AT_EGID && !AT_SECURE`를
   전부 0으로 둬 `__init_libc`의 `poll()` 기반 stdio 보안 검사(이
   커널엔 `SYS_poll`이 없다)를 항상 건너뛰게 만들었다. **기존
   호출자(initrun+14개 서버) 전부는 이 필드를 생략(zero-init 기본값
   false)하므로 전혀 영향받지 않는다** — musl-hello 전용이다.

이 두 발견 덕분에 M30("TLS/`arch_prctl`")의 절반은 이미 M28로
흡수됐고, M29("유저 스택에 최소 auxv 구성")도 "새로 만드는" 것이
아니라 "이번에 채운 고정 최소값을 PT_INTERP에 필요한 실제 값으로
확장하는" 작업으로 범위가 좁아진다.

## 그 외 겪은 문제 — git apply의 CRLF 정규화가 패치를 조용히 무시함

`tools/apply-patches.sh`의 첫 구현은 `git apply`를 썼다. 이
저장소의 `core.autocrlf=true` 설정 때문에 (1) `git diff --no-index`가
생성하는 패치 자체가 CRLF를 전부 LF로 정규화해 버리고, (2)
`git apply`가 CRLF로 체크아웃된 `third_party/musl` 원본 파일에
대해 그 패치를 적용할 때도 같은 정규화를 시도하다 "Skipped patch"로
**아무 것도 바꾸지 않고 성공(exit 0)을 반환**하는 것을 실제로
겪었다 — 겉보기엔 정상 종료라 처음엔 원인을 못 찾았다(패치 파일의
CRLF를 원본과 맞춰 봐도 `git diff --no-index` 자신이 다시 LF로
되돌려 버렸다). Python의 `difflib.unified_diff`로 패치 자체를 순수
LF로 생성하고, 적용 단계는 `git apply` 대신 표준 `patch(1)`
유틸리티로 바꿔 완전히 해결했다(`patch(1)`은 이런 정규화를 하지
않는다, 그리고 C 컴파일러는 소스 파일의 줄바꿈 방식에 영향받지
않는다).

## 검증

x86_64 전체 재빌드 성공(58개 이상 타깃 — musl-hello elf 신규 포함).
bootdisk 재생성. QEMU에서 **"hello from real musl"**이 실제로
출력됨을 확인했다 — 부팅 로그 순서: `[musl-hello] find musl_hello
ok=1` → `[process] spawn ok` → `[musl-hello] spawn err=0` →
(다른 M9~M27 데모들이 인터리브되며 실행) → `[syscall_shim]
unimplemented n=218`(`SYS_set_tid_address`, musl이 실패를 무시하고
계속 진행하도록 이미 설계돼 있어 크래시하지 않음을 확인) →
`hello from real musl` → 이후 initrun/procsrv 등 나머지 부팅이
평소와 동일하게 계속됨(크래시·회귀 없음). QEMU 5개 회귀 스위트
전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0) — 신규 확인 문자열(musl-hello spawn/hello) 포함 |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0) |

## 남겨 둔 것

real-libc-syscall-layer.md M29(동적 링킹 도입)부터 계속 진행한다.
M30의 TLS 부분은 이미 완료됐으므로 M30 착수 시점에는
`SYS_mmap`/`SYS_munmap`(musl 자신의 malloc, mallocng)만 남는다.
