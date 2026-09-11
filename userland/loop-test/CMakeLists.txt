# docs/plan/musl-userland-porting.md §M55 (ADR-227) — msh의 job
# control 자기테스트 대상. userland/echo/CMakeLists.txt와 완전히
# 같은 설정(진짜 musl 프로그램).
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_loop_test INTERFACE)
  return()
endif()

add_executable(minicore_loop_test_elf
    main.c
)

target_link_libraries(minicore_loop_test_elf PRIVATE minicore_libc minicore_libmc)

target_compile_options(minicore_loop_test_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_C_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_loop_test_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_loop_test_elf PROPERTIES
    LINKER_LANGUAGE C
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
