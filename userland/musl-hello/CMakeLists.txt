# musl로 실제로 링크된 최초의 유저 프로그램(real-libc-syscall-layer.md
# §M28). init/initrun/CMakeLists.txt와 같은 이유로 x86_64 전용이다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_musl_hello INTERFACE)
  return()
endif()

add_executable(minicore_musl_hello_elf
    main.c
)

target_link_libraries(minicore_musl_hello_elf PRIVATE minicore_libc minicore_libmc)

target_compile_options(minicore_musl_hello_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_C_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_musl_hello_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_musl_hello_elf PROPERTIES
    LINKER_LANGUAGE C
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
