# 익명 파이프+dup2() 검증용 실제 musl 프로그램(docs/plan/
# musl-userland-porting.md §M51). userland/musl-hello/CMakeLists.txt
# 와 완전히 같은 설정.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_pipe_test INTERFACE)
  return()
endif()

add_executable(minicore_pipe_test_elf
    main.c
)

target_link_libraries(minicore_pipe_test_elf PRIVATE minicore_libc minicore_libmc)

target_compile_options(minicore_pipe_test_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_C_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_pipe_test_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_pipe_test_elf PROPERTIES
    LINKER_LANGUAGE C
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
