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
# 지금은 arch/x86_64/syscall_arch.h 하나만 실제로 바뀌지만, 앞으로
# 추가되는 패치도 전부 이 경로를 통해 자동으로 반영된다.
set(MUSL_PATCHED_ROOT ${CMAKE_CURRENT_BINARY_DIR}/_patched/musl)
set(MUSL_PATCHED_STAMP ${MUSL_PATCHED_ROOT}/.stamp)

add_custom_command(
    OUTPUT ${MUSL_PATCHED_STAMP}
    COMMAND bash ${CMAKE_SOURCE_DIR}/tools/apply-patches.sh musl ${MUSL_PATCHED_ROOT}
    COMMAND ${CMAKE_COMMAND} -E touch ${MUSL_PATCHED_STAMP}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/apply-patches.sh
            ${CMAKE_SOURCE_DIR}/third_party/patches/musl/0001-syscall-shim.patch
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
)
add_dependencies(minicore_libc minicore_libc_musl_alltypes minicore_libc_musl_syscall_bits
                  minicore_libc_musl_patched)

# musl 자신의 Makefile이 쓰는 것과 같은 include 경로 목록.
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
target_include_directories(minicore_libc PRIVATE
    ${MUSL_PATCHED_ROOT}/src/include
    ${MUSL_PATCHED_ROOT}/src/internal
)
# 패치된 트리(arch/x86_64)를 원본보다 먼저 넣는다 — #include
# "syscall_arch.h" 등이 패치된 버전을 먼저 찾게 한다(M28, 위 add_library
# 주석 참고). 지금은 arch/x86_64 하나만 실제로 다르지만, 나머지
# (arch/generic, include/)도 패치 파이프라인의 일부로 통일해 둔다.
target_include_directories(minicore_libc PUBLIC
    ${MUSL_PATCHED_ROOT}/arch/x86_64
    ${MUSL_PATCHED_ROOT}/arch/generic
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
