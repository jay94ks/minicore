# 로그인 후 셸 (docs/plan/system-servers-bringup.md §M20,
# docs/design/foundations.md ADR-170 — 포팅이 아니라 minicore
# 전용 대체 구현, ADR-049). libc 없이 libmc(순수 C)만 링크한다.
# init/initrun/CMakeLists.txt와 같은 이유로 x86_64 전용이다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_shell INTERFACE)
  return()
endif()

add_executable(minicore_shell_elf
    main.c
)

target_link_libraries(minicore_shell_elf PRIVATE minicore_libmc)

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
