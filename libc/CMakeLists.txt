# POSIX 계층 (ADR-005/008). M26(general-purpose-completion.md §M26,
# docs/design/foundations.md ADR-182)이 채택을 확정한다 — musl
# (third_party/musl, v1.2.6 고정, ADR-022) 소스 중 **문자열 함수만**
# 무수정으로 가져와 실제로 빌드한다. 전체 포팅(진짜 syscall 계층,
# 동적 링커, 스레드 등)은 이 라운드 범위 밖이다 — ADR-182 "알려진
# 단순화" 참고. x86_64 전용이다(init/initrun/CMakeLists.txt와 같은
# 이유 — third_party/musl/arch/x86_64가 다른 아키텍처 이식을 아직
# 다루지 않는다).
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

find_package(Python3 COMPONENTS Interpreter REQUIRED)

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

# ADR-182가 고른 부분집합 — 문자열 함수 중 syscall/스레드/락 없이도
# 완결되는 것들만(strdup는 예외로, malloc 하나만 필요하다 — 아래
# mem_shim.c가 그 자리를 메운다). __stpcpy/__strchrnul/__memrchr처럼
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
    sysdeps/minicore/mem_shim.c
)
add_dependencies(minicore_libc minicore_libc_musl_alltypes)

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
    ${MUSL_ROOT}/src/include
    ${MUSL_ROOT}/src/internal
)
target_include_directories(minicore_libc PUBLIC
    ${MUSL_ROOT}/arch/x86_64
    ${MUSL_ROOT}/arch/generic
    ${MUSL_GENERATED_INCLUDE}
    ${MUSL_ROOT}/include
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
