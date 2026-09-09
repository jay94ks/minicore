# 로그인 후 셸 (docs/plan/system-servers-bringup.md §M20,
# docs/design/foundations.md ADR-170 — 포팅이 아니라 minicore
# 전용 대체 구현, ADR-049). libmc(순수 C, 커널 syscall/IPC 클라이언트)
# 는 그대로 쓰고, M26(ADR-182)부터 문자열 함수는 third_party/musl
# 원본 소스로 빌드한 minicore_libc를 추가로 링크한다(main.c의
# <string.h> 사용). init/initrun/CMakeLists.txt와 같은 이유로
# x86_64 전용이다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_shell INTERFACE)
  return()
endif()

add_executable(minicore_shell_elf
    main.c
)

target_link_libraries(minicore_shell_elf PRIVATE minicore_libmc minicore_libc)

target_compile_options(minicore_shell_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_C_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_shell_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_shell_elf PROPERTIES
    LINKER_LANGUAGE C
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
