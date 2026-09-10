# POSIX 계층 (ADR-005/008). M26(general-purpose-completion.md §M26,
# docs/design/foundations.md ADR-182)이 문자열 함수 부분집합만 먼저
# 채택했고, M28(real-libc-syscall-layer.md §M28, ADR-183)이 실제
# syscall 계층 포팅에 착수한다 — musl의 진짜 시작 경로(crt1.c,
# __libc_start_main.c, __init_tls.c)+최소 syscall 집합(write/exit/
# arch_prctl)이 이 라운드에서 추가된다. x86_64 전용이다(init/initrun/
# CMakeLists.txt와 같은 이유 — third_party/musl/arch/x86_64가 다른
# 아키텍처 이식을 아직 다루지 않는다).
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_libc_glue INTERFACE)
  add_library(minicore_libc INTERFACE)
  return()
endif()

add_library(minicore_libc_glue INTERFACE)
target_include_directories(minicore_libc_glue INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/sysdeps/minicore
)

set(MUSL_ROOT ${CMAKE_SOURCE_DIR}/third_party/musl)
set(MUSL_GENERATED_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(MUSL_ALLTYPES_H ${MUSL_GENERATED_INCLUDE}/bits/alltypes.h)
set(MUSL_SYSCALL_BITS_H ${MUSL_GENERATED_INCLUDE}/bits/syscall.h)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# M28(real-libc-syscall-layer.md §M28, ADR-183) — third_party/patches/musl/
# *.patch를 실제로 적용한 작업 트리(tools/apply-patches.sh, ADR-022가
# 예약해 둔 자리를 처음 실제로 씀). 이 아래 minicore_libc의 모든
# musl 소스 include 경로는 원본이 아니라 이 패치된 트리를 가리킨다 —
# 0001은 arch/x86_64/syscall_arch.h(모든 __syscallN을 우회), 0002는
# src/signal/x86_64/restore.s를 바꾼다. restore.s는 __syscallN을
# 거치지 않는 특수 경로다 — 시그널 핸들러가 ret한 뒤 CPU가 곧바로
# 뛰어드는 손짜기 트램폴린이라(진짜 Linux도 이 트램폴린은 raw
# `syscall`을 직접 쓴다, C 러너타임을 아예 안 거친다) 0001의 우회가
# 적용될 자리가 없다 — 원본은 Linux ABI 그대로 번호를 RAX에 싣지만
# (mov $15,%rax), 이 커널의 syscall_entry는 번호를 RDI에서 읽는다
# (syscall_entry.S 상단 주석) — 그대로 두면 syscall_dispatch가 핸들러
# 실행 후 RDI에 남은 임의 값을 번호로 오인해 rt_sigreturn이 전혀
# 실행되지 않는다(M36에서 실제로 겪음: #GP로 죽었고, 죽은 위치가
# 정확히 __restore_rt의 syscall 다음 명령이었다, 2026-09-10). 0002는
# 그 한 줄을 이 커널의 번호(MC_SYSCALL_RT_SIGRETURN=21, mc/syscall.h)
# 를 RDI에 싣도록 바꾼다 — sigreturn은 인자가 없어 RSI/RDX/R10에
# 남는 값은 무해하다.
set(MUSL_PATCHED_ROOT ${CMAKE_CURRENT_BINARY_DIR}/_patched/musl)
set(MUSL_PATCHED_STAMP ${MUSL_PATCHED_ROOT}/.stamp)

add_custom_command(
    OUTPUT ${MUSL_PATCHED_STAMP}
    COMMAND bash ${CMAKE_SOURCE_DIR}/tools/apply-patches.sh musl ${MUSL_PATCHED_ROOT}
    COMMAND ${CMAKE_COMMAND} -E touch ${MUSL_PATCHED_STAMP}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/apply-patches.sh
            ${CMAKE_SOURCE_DIR}/third_party/patches/musl/0001-syscall-shim.patch
            ${CMAKE_SOURCE_DIR}/third_party/patches/musl/0002-restore-trampoline.patch
    COMMENT "Applying musl patches (tools/apply-patches.sh) -> ${MUSL_PATCHED_ROOT}"
    VERBATIM
)
add_custom_target(minicore_libc_musl_patched DEPENDS ${MUSL_PATCHED_STAMP})

# CMake는 add_library에 나열하는 소스 파일이 configure 시점에 실제로
# 존재해야 한다고 요구한다 — 이 파일들은 위 커스텀 커맨드가 build
# 시점에야 만든다(apply-patches.sh는 stamp 파일 하나만 OUTPUT으로
# 선언한다, 트리 전체를 개별 OUTPUT으로 나열하지 않는다). GENERATED로
# 표시해 이 요구를 우회한다("빌드 규칙이 나중에 만들어 준다"는 뜻).
set(MUSL_PATCHED_M28_SOURCES
    ${MUSL_PATCHED_ROOT}/crt/crt1.c
    ${MUSL_PATCHED_ROOT}/src/env/__libc_start_main.c
    ${MUSL_PATCHED_ROOT}/src/env/__init_tls.c
    ${MUSL_PATCHED_ROOT}/src/env/__environ.c
    ${MUSL_PATCHED_ROOT}/src/internal/syscall_ret.c
    ${MUSL_PATCHED_ROOT}/src/internal/libc.c
    ${MUSL_PATCHED_ROOT}/src/internal/defsysinfo.c
    ${MUSL_PATCHED_ROOT}/src/thread/default_attr.c
    ${MUSL_PATCHED_ROOT}/src/thread/__syscall_cp.c
    ${MUSL_PATCHED_ROOT}/src/errno/__errno_location.c
    ${MUSL_PATCHED_ROOT}/src/exit/exit.c
    ${MUSL_PATCHED_ROOT}/src/exit/_Exit.c
    ${MUSL_PATCHED_ROOT}/src/unistd/write.c
    ${MUSL_PATCHED_ROOT}/src/unistd/_exit.c
)
set_source_files_properties(${MUSL_PATCHED_M28_SOURCES} PROPERTIES GENERATED TRUE)

# musl 자신의 Makefile이 `sed -f tools/mkalltypes.sed arch/$(ARCH)/bits/
# alltypes.h.in include/alltypes.h.in > bits/alltypes.h`로 만드는 그
# 한 파일을 재현한다(ADR-182 참고 — sed 대신 파이썬으로 재구현해
# 이 프로젝트의 다른 생성 스텝들과 셸 의존성을 통일했다, 변환 결과는
# 줄바꿈 차이만 있고 내용은 동일함을 확인했다).
add_custom_command(
    OUTPUT ${MUSL_ALLTYPES_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_GENERATED_INCLUDE}/bits
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen-musl-alltypes.py
            ${MUSL_ALLTYPES_H}
            ${MUSL_ROOT}/arch/x86_64/bits/alltypes.h.in
            ${MUSL_ROOT}/include/alltypes.h.in
    DEPENDS ${MUSL_ROOT}/arch/x86_64/bits/alltypes.h.in
            ${MUSL_ROOT}/include/alltypes.h.in
            ${CMAKE_SOURCE_DIR}/tools/gen-musl-alltypes.py
    COMMENT "Generating musl bits/alltypes.h for x86_64 (tools/gen-musl-alltypes.py)"
    VERBATIM
)
add_custom_target(minicore_libc_musl_alltypes DEPENDS ${MUSL_ALLTYPES_H})

# musl 자신의 Makefile이 `cp arch/$(ARCH)/bits/syscall.h.in bits/syscall.h;
# sed -n -e s/__NR_/SYS_/p ... >> bits/syscall.h`로 만드는 파일을
# 재현한다(tools/gen-musl-syscall-bits.py — 위 alltypes와 같은 이유로
# 파이썬 재구현). musl의 <sys/syscall.h>(src/internal/syscall.h가
# include)가 이 파일을 요구한다 — M28부터 SYS_write 등 실제 syscall
# 번호가 필요해져 처음 생성한다.
add_custom_command(
    OUTPUT ${MUSL_SYSCALL_BITS_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSL_GENERATED_INCLUDE}/bits
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen-musl-syscall-bits.py
            ${MUSL_SYSCALL_BITS_H}
            ${MUSL_ROOT}/arch/x86_64/bits/syscall.h.in
    DEPENDS ${MUSL_ROOT}/arch/x86_64/bits/syscall.h.in
            ${CMAKE_SOURCE_DIR}/tools/gen-musl-syscall-bits.py
    COMMENT "Generating musl bits/syscall.h for x86_64 (tools/gen-musl-syscall-bits.py)"
    VERBATIM
)
add_custom_target(minicore_libc_musl_syscall_bits DEPENDS ${MUSL_SYSCALL_BITS_H})

# ADR-182가 고른 부분집합 — 문자열 함수 중 syscall/스레드/락 없이도
# 완결되는 것들만(strdup는 예외로, malloc 하나만 필요하다 — 아래
# lite_malloc.c(M30)가 그 자리를 메운다). __stpcpy/__strchrnul/__memrchr처럼
# 다른 함수가 내부적으로 요구하는 지원 함수도 함께 담는다.
add_library(minicore_libc STATIC
    ${MUSL_ROOT}/src/string/memcpy.c
    ${MUSL_ROOT}/src/string/memmove.c
    ${MUSL_ROOT}/src/string/memset.c
    ${MUSL_ROOT}/src/string/memcmp.c
    ${MUSL_ROOT}/src/string/memchr.c
    ${MUSL_ROOT}/src/string/memrchr.c
    ${MUSL_ROOT}/src/string/strcmp.c
    ${MUSL_ROOT}/src/string/strncmp.c
    ${MUSL_ROOT}/src/string/strcpy.c
    ${MUSL_ROOT}/src/string/stpcpy.c
    ${MUSL_ROOT}/src/string/strncpy.c
    ${MUSL_ROOT}/src/string/strcat.c
    ${MUSL_ROOT}/src/string/strncat.c
    ${MUSL_ROOT}/src/string/strchr.c
    ${MUSL_ROOT}/src/string/strchrnul.c
    ${MUSL_ROOT}/src/string/strrchr.c
    ${MUSL_ROOT}/src/string/strspn.c
    ${MUSL_ROOT}/src/string/strcspn.c
    ${MUSL_ROOT}/src/string/strlen.c
    ${MUSL_ROOT}/src/string/strdup.c

    # M28(real-libc-syscall-layer.md §M28, ADR-183) — musl의 진짜
    # 시작 경로 + 최소 syscall 집합(write/exit/arch_prctl). 이
    # 파일들은 원본이 아니라 패치된 트리(MUSL_PATCHED_ROOT, 위
    # MUSL_PATCHED_M28_SOURCES 정의부 주석 참고)에서 가져온다.
    ${MUSL_PATCHED_M28_SOURCES}
    sysdeps/minicore/syscall_shim.c
    sysdeps/minicore/set_thread_area.c

    # M30(real-libc-syscall-layer.md §M30, ADR-183) — musl 자신의
    # 진짜 malloc(lite_malloc.c, 순수 SYS_mmap 기반 범프 할당자 —
    # M26의 mem_shim.c 대체, ADR-183 계획대로). lite_malloc.c가
    # 먼저 SYS_brk를 시도하지만 syscall_shim.c가 항상 실패로
    # 답해(위 syscall_shim.c 주석 참고) 무조건 mmap 경로로 우회한다.
    ${MUSL_PATCHED_ROOT}/src/malloc/lite_malloc.c
    ${MUSL_PATCHED_ROOT}/src/malloc/calloc.c
    ${MUSL_PATCHED_ROOT}/src/malloc/free.c
    ${MUSL_PATCHED_ROOT}/src/malloc/replaced.c
    ${MUSL_PATCHED_ROOT}/src/mman/mmap.c
    ${MUSL_PATCHED_ROOT}/src/mman/munmap.c
    sysdeps/minicore/lock_shim.c
    sysdeps/minicore/malloc_shim.c

    # M31(real-libc-syscall-layer.md §M31, ADR-183) — musl 자신의
    # 진짜 stdio(fopen/fread/fclose+printf, 재구현이 아니라 musl
    # 소스 자체). 읽기 경로(fopen/__fdopen/__fmodeflags, fread/
    # __stdio_read/__toread/__uflow, fclose/__stdio_close/
    # __stdio_seek)+출력 경로(stdout/stderr/stdin, __stdout_write/
    # __stdio_write/__towrite/__overflow, printf/vfprintf)+공통
    # 인프라(ofl/ofl_add, __stdio_exit — exit()가 이미 참조하는
    # weak dummy를 이제 진짜로 덮어쓴다).
    ${MUSL_PATCHED_ROOT}/src/stdio/fopen.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__fdopen.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__fmodeflags.c
    ${MUSL_PATCHED_ROOT}/src/stdio/fread.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__stdio_read.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__toread.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__uflow.c
    ${MUSL_PATCHED_ROOT}/src/stdio/fclose.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__stdio_close.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__stdio_seek.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__stdio_exit.c
    ${MUSL_PATCHED_ROOT}/src/stdio/ofl.c
    ${MUSL_PATCHED_ROOT}/src/stdio/ofl_add.c
    ${MUSL_PATCHED_ROOT}/src/stdio/stdout.c
    ${MUSL_PATCHED_ROOT}/src/stdio/stderr.c
    ${MUSL_PATCHED_ROOT}/src/stdio/stdin.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__stdout_write.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__stdio_write.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__towrite.c
    ${MUSL_PATCHED_ROOT}/src/stdio/__overflow.c
    ${MUSL_PATCHED_ROOT}/src/stdio/printf.c
    ${MUSL_PATCHED_ROOT}/src/stdio/vfprintf.c
    ${MUSL_PATCHED_ROOT}/src/stdio/fflush.c
    ${MUSL_PATCHED_ROOT}/src/stdio/fwrite.c
    ${MUSL_PATCHED_ROOT}/src/unistd/lseek.c

    # M31 — vfprintf.c(printf_core)의 나머지 링크 의존성. 이 라운드의
    # 실제 사용(%s/%zu류 정수·문자열 서식)은 이 중 대부분을 런타임에
    # 타지 않지만(예: 부동소수점 서식·와이드 문자), vfprintf.c는
    # 모든 서식 지정자를 한 파일에 같이 담고 있어 링크 시점에는
    # 전부 필요하다.
    ${MUSL_PATCHED_ROOT}/src/ctype/isdigit.c
    ${MUSL_PATCHED_ROOT}/src/ctype/__ctype_get_mb_cur_max.c
    ${MUSL_PATCHED_ROOT}/src/errno/strerror.c
    ${MUSL_PATCHED_ROOT}/src/locale/__lctrans.c
    ${MUSL_PATCHED_ROOT}/src/string/strnlen.c
    ${MUSL_PATCHED_ROOT}/src/multibyte/wctomb.c
    ${MUSL_PATCHED_ROOT}/src/multibyte/wcrtomb.c
    ${MUSL_PATCHED_ROOT}/src/math/frexpl.c
    ${MUSL_PATCHED_ROOT}/src/math/scalbn.c
    ${MUSL_PATCHED_ROOT}/src/math/__fpclassifyl.c
    ${MUSL_PATCHED_ROOT}/src/math/__signbitl.c

    # M32(real-libc-syscall-layer.md §M32, ADR-183) — musl 자신의
    # 진짜 fork()/execve()/waitpid()/getpid(). fork()는 _Fork()로
    # 내려가고, _Fork()는 x86_64에서 SYS_clone이 아니라 SYS_fork를
    # 직접 쓴다(third_party/musl/src/process/_Fork.c의
    # `#ifdef SYS_fork` 분기).
    ${MUSL_PATCHED_ROOT}/src/process/fork.c
    ${MUSL_PATCHED_ROOT}/src/process/_Fork.c
    ${MUSL_PATCHED_ROOT}/src/process/execve.c
    ${MUSL_PATCHED_ROOT}/src/process/waitpid.c
    ${MUSL_PATCHED_ROOT}/src/unistd/getpid.c

    # M32 — fork()/_Fork()의 링크 의존성. abort_lock.c는 순수 전역
    # 배열이라 그대로 쓴다. block.c(__block_all_sigs/__block_app_sigs/
    # __restore_sigs)는 SYS_rt_sigprocmask를 부르는데, 이 syscall은
    # 아직 미구현이라 syscall_shim.c의 기본 정책대로 -ENOSYS+로그로
    # 처리된다 — 반환값을 확인하지 않는 void 함수라 무해하다(신호
    # 마스크 자체가 아직 이 커널에 없다, 완전한 signal 계층은 M36
    # 대상). __inhibit_ptc/__acquire_ptc/__release_ptc(원본은
    # pthread_rwlock 기반)는 sysdeps/minicore/lock_shim.c의 같은
    # 이유(단일 스레드, 아직 pthread_create 없음, M37 대상)로 no-op
    # 대체한다 — 그래서 원본 src/thread/lock_ptc.c는 여기 없다.
    ${MUSL_PATCHED_ROOT}/src/exit/abort_lock.c
    ${MUSL_PATCHED_ROOT}/src/signal/block.c

    # M36(real-libc-syscall-layer.md §M36, ADR-186) — 완전한 signal
    # 계층. musl 자신의 진짜 sigaction()(SYS_rt_sigaction을 부른다,
    # third_party/musl/src/signal/sigaction.c)이 이번에 처음
    # 필요해졌다.
    ${MUSL_PATCHED_ROOT}/src/signal/sigaction.c
    ${MUSL_PATCHED_ROOT}/src/signal/x86_64/restore.s

    # M36 — musl-hello의 fork() 자기테스트가 부모/자식 사이 실제
    # 동기화 수단이 없어(COW라 서로의 쓰기가 보이지 않는다) 부모가
    # sched_yield()를 반복 호출해 스케줄러에게 자식을 실행할 기회를
    # 강제로 준다(syscall_shim.c::SYS_sched_yield, mc/syscall.h::
    # MC_SYSCALL_YIELD 주석 참고).
    ${MUSL_PATCHED_ROOT}/src/sched/sched_yield.c

    # M37(real-libc-syscall-layer.md §M37, ADR-187) — pthread 최소
    # 구현. musl 자신의 진짜 pthread_create()+pthread_join()+
    # pthread_mutex_lock/unlock/trylock/timedlock이 이번에 처음
    # 필요해졌다. __clone(third_party/musl/src/thread/x86_64/clone.s)
    # 은 M36의 restore.s와 같은 이유(raw Linux ABI 직접 사용, 이
    # 커널의 syscall ABI와 불일치)로 원본을 쓰지 않고
    # sysdeps/minicore/clone_shim.c(순수 C 대체, 이 커널에서는 그
    # asm 트릭이 애초에 필요 없다)로 완전히 갈아 끼운다 — clone.s
    # 자체는 소스 목록에 없다(third_party 원본을 고치는 대신, 링커가
    # 이 .c 파일이 정의한 같은 이름의 심벌을 대신 쓰게 한다).
    # __lock/__unlock은 M30/M31이 no-op으로 미뤄 뒀던 것을 이번에
    # 진짜 구현(lock_shim.c 주석 참고)으로 되돌린다.
    ${MUSL_PATCHED_ROOT}/src/thread/pthread_create.c
    ${MUSL_PATCHED_ROOT}/src/thread/pthread_join.c
    ${MUSL_PATCHED_ROOT}/src/thread/pthread_mutex_lock.c
    ${MUSL_PATCHED_ROOT}/src/thread/pthread_mutex_unlock.c
    ${MUSL_PATCHED_ROOT}/src/thread/pthread_mutex_trylock.c
    ${MUSL_PATCHED_ROOT}/src/thread/pthread_mutex_timedlock.c
    ${MUSL_PATCHED_ROOT}/src/thread/__timedwait.c
    ${MUSL_PATCHED_ROOT}/src/thread/pthread_setcancelstate.c
    ${MUSL_PATCHED_ROOT}/src/thread/pthread_testcancel.c
    ${MUSL_PATCHED_ROOT}/src/thread/__unmapself.c
    ${MUSL_PATCHED_ROOT}/src/thread/__lock.c
    ${MUSL_PATCHED_ROOT}/src/thread/__wait.c
    ${MUSL_PATCHED_ROOT}/src/thread/vmlock.c
    ${MUSL_PATCHED_ROOT}/src/time/clock_gettime.c
    ${MUSL_PATCHED_ROOT}/src/mman/mprotect.c
    ${MUSL_PATCHED_ROOT}/src/misc/syscall.c

    # M35(real-libc-syscall-layer.md §M35, ADR-188) — setlocale()과
    # 그 링크 의존성(locale_map.c::__get_locale, c_locale.c::
    # __c_dot_utf8/__c_locale, __mo_lookup.c::__mo_lookup — __lctrans_impl
    # 이 부른다). __map_file(MUSL_LOCPATH 환경변수 경로 탐색 — 이
    # 프로젝트의 envp는 항상 비어 있어(M28) 실제로는 절대 실행되지
    # 않는 경로)은 원본(third_party/musl/src/time/__map_file.c,
    # open+fstat+mmap로 진짜 로케일 아카이브를 매핑) 대신
    # sysdeps/minicore/locale_shim.c의 항상-실패 대체를 쓴다 —
    # lock_shim.c/malloc_shim.c와 같은 이유(실행되지 않을 코드를
    # 위해 fstat 등 새 의존성을 끌어올 이유가 없다). getenv/toupper/
    # tolower는 ctype/env 표준 함수 — 이번에 처음 필요해졌다.
    ${MUSL_PATCHED_ROOT}/src/locale/setlocale.c
    ${MUSL_PATCHED_ROOT}/src/locale/locale_map.c
    ${MUSL_PATCHED_ROOT}/src/locale/c_locale.c
    ${MUSL_PATCHED_ROOT}/src/locale/__mo_lookup.c
    ${MUSL_PATCHED_ROOT}/src/env/getenv.c
    ${MUSL_PATCHED_ROOT}/src/ctype/toupper.c
    ${MUSL_PATCHED_ROOT}/src/ctype/tolower.c
    ${MUSL_PATCHED_ROOT}/src/ctype/isupper.c
    ${MUSL_PATCHED_ROOT}/src/ctype/islower.c
    sysdeps/minicore/locale_shim.c
    sysdeps/minicore/clone_shim.c
)
add_dependencies(minicore_libc minicore_libc_musl_alltypes minicore_libc_musl_syscall_bits
                  minicore_libc_musl_patched)

# M31(real-libc-syscall-layer.md §M31) — musl 자신의 Makefile이
# 항상 쓰는 플래그(third_party/musl/Makefile CFLAGS_ALL)를 그대로
# 맞춘다. 이게 없으면 include/unistd.h의 `long syscall(long, ...);`
# 선언(_GNU_SOURCE||_BSD_SOURCE 뒤에서만 노출)이 안 보여야 하는데,
# src/internal/syscall.h가 이미 `#define syscall(...) ...` 매크로를
# 정의해 둔 상태에서 그 선언 줄의 `syscall` 토큰이 매크로로 치환돼
# 깨진 선언이 되는 것을 실제로 겪었다(__stdio_seek.c가 stdio_impl.h
# 다음에 <unistd.h>를 include) — _XOPEN_SOURCE=700만으로는 그 가드를
# 열지 않아 애초에 이 충돌이 안 난다(2026-09-10).
target_compile_definitions(minicore_libc PRIVATE _XOPEN_SOURCE=700)

# vfprintf.c(M31, printf의 %f/%e/%g 부동소수점 서식)는 double 반환값을
# SysV x86_64 관례대로 XMM0로 돌려받는 코드를 생성한다 — 이 파일
# 하나만 SSE를 켠다(나머지 musl 소스는 계속 -mno-sse, 아래 참고).
# 이 커널이 이미 M9~M11b에서 완성한 유저 스레드별 FPU/SSE 컨텍스트
# 스위칭(lazy XSAVE 포함) 덕분에 안전하다 — musl-hello도 평범한 유저
# 스레드이므로 처음 SSE를 쓰는 순간 자동으로 #NM 경로를 타 등록된다.
set_source_files_properties(
    ${MUSL_PATCHED_ROOT}/src/stdio/vfprintf.c
    ${MUSL_PATCHED_ROOT}/src/math/frexpl.c
    ${MUSL_PATCHED_ROOT}/src/math/scalbn.c
    ${MUSL_PATCHED_ROOT}/src/math/__fpclassifyl.c
    ${MUSL_PATCHED_ROOT}/src/math/__signbitl.c
    PROPERTIES COMPILE_OPTIONS "-msse;-msse2"
)

# musl 자신의 Makefile이 쓰는 것과 같은 include 경로 목록 — 순서까지
# 그대로 맞춘다(third_party/musl/Makefile:50의 CFLAGS_ALL 참고:
# arch/$(ARCH) → arch/generic → obj/src/internal → src/include →
# src/internal → obj/include → include). arch/x86_64가 src/internal
# 보다 **먼저** 와야 한다 — 그렇지 않으면 `#include "ksigaction.h"`
# 같은 "같은 이름, arch별 오버라이드 있음" 헤더가 arch 버전이
# 아니라 src/internal의 제네릭 버전으로 잡힌다(M36에서 실제로 겪음,
# 2026-09-10: src/internal/ksigaction.h는 `__restore`/`__restore_rt`를
# 별도 심벌 두 개로 선언하고, arch/x86_64/ksigaction.h만
# `#define __restore __restore_rt`로 별칭 처리한다 — 순서가 뒤바뀌면
# sigaction.c가 존재하지 않는 `__restore` 심벌을 참조해 링크가
# 깨진다). arch/x86_64(패치된 트리)를 원본보다 먼저 넣는 것도 이미
# 이 순서의 일부다 — #include "syscall_arch.h" 등이 패치된 버전을
# 먼저 찾게 한다(M28, 위 add_library 주석 참고).
#
# src/include·src/internal(weak_alias 등)은 minicore_libc 자신의
# .c 파일을 빌드할 때만 필요해 PRIVATE다. 나머지(arch/x86_64,
# arch/generic, 생성 헤더, 공개 include/)는 **PUBLIC**이어야
# 한다 — musl의 공개 헤더(string.h 등)는 내부적으로 <stdint.h> →
# <bits/alltypes.h>뿐 아니라 <bits/stdint.h>(arch/generic, 정적
# 파일 — musl이 x86_64용 별도 재정의를 안 둬서 generic을 그대로
# 쓴다)처럼 arch별 bits/*.h를 더 참조한다 — 이 헤더들을 실제로
# 쓰는 쪽은 musl의 .c 파일 자신뿐이 아니라 musl의 string.h를 쓰는
# **소비자**(예: userland/shell)의 컴파일 단위 전체다. 하나라도
# PRIVATE로 남으면 소비자 쪽에서 그 헤더를 못 찾아 빌드가 깨진다
# (실제로 bits/alltypes.h, 그다음 bits/stdint.h 순서로 두 번 겪음,
# 2026-09-10).
#
# 위 순서 주석대로 PUBLIC/PRIVATE 두 그룹으로만 나누면 안 된다 —
# real musl 순서는 arch/* 다음에 src/include·src/internal이 오고
# **그 다음에** 생성된 공개 헤더(obj/include)·공개 include/가
# 온다(예: <string.h>가 src/internal에도, 공개 include/에도 있고,
# musl 자신의 .c는 __memrchr/__strchrnul 같은 내부 전용 선언이
# 추가된 src/internal 버전을 먼저 봐야 한다 — 공개 버전이 먼저
# 잡히면 그 내부 심벌들이 전부 "선언 없음"이 된다, M36에서
# ksigaction.h 순서를 고치며 함께 겪음, 2026-09-10). 그래서 arch
# 그룹과 생성/공개 그룹을 별도 PUBLIC 호출로 쪼개 그 사이에
# PRIVATE(src/include, src/internal) 호출을 끼워 넣는다.
target_include_directories(minicore_libc PUBLIC
    ${MUSL_PATCHED_ROOT}/arch/x86_64
    ${MUSL_PATCHED_ROOT}/arch/generic
)
target_include_directories(minicore_libc PRIVATE
    ${MUSL_PATCHED_ROOT}/src/include
    ${MUSL_PATCHED_ROOT}/src/internal
)
target_include_directories(minicore_libc PUBLIC
    ${MUSL_GENERATED_INCLUDE}
    ${MUSL_PATCHED_ROOT}/include
)

target_link_libraries(minicore_libc PRIVATE minicore_libmc)

target_compile_options(minicore_libc PRIVATE
    # third_party 원본은 무수정으로 유지한다(ADR-022) — 우리가 그
    # 코드 스타일을 린트할 대상이 아니다(공통 -Wall -Wextra를 여기서
    # 끈다, 뒤에 오는 플래그가 이긴다).
    -w
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_C_STANDARD 11)
