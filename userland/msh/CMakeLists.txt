# docs/plan/musl-userland-porting.md §M52 (ADR-221) — 자체 작성
# 최소 coreutils 중 하나. userland/musl-hello/CMakeLists.txt와
# 완전히 같은 설정(진짜 musl 프로그램).
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_msh INTERFACE)
  return()
endif()

add_executable(minicore_msh_elf
    main.c
)

target_link_libraries(minicore_msh_elf PRIVATE minicore_libc minicore_libmc)

target_compile_options(minicore_msh_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_C_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_msh_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_msh_elf PROPERTIES
    LINKER_LANGUAGE C
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
