# 익명 파이프 서버 (docs/plan/musl-userland-porting.md §M51). servers/
# svcmgr/CMakeLists.txt와 같은 설정(libk+libmc만 링크, 순수 네이티브,
# musl 불필요) — ELF 임베딩이 필요 없다(이 서버는 다른 프로세스를
# spawn하지 않는다).
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_pipesrv INTERFACE)
  return()
endif()

add_executable(minicore_pipesrv_elf
    main.cpp
)

target_include_directories(minicore_pipesrv_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/mc/include
)

target_link_libraries(minicore_pipesrv_elf PRIVATE minicore_libk minicore_libmc)

target_compile_options(minicore_pipesrv_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_pipesrv_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_pipesrv_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
