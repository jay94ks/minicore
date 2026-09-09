# 네트워크 서버: 소켓·프로토콜 스택 (ADR-006/008).
# M25(docs/plan/general-purpose-completion.md §M25)에서 처음으로
# 실제 코드를 채운다 — main.cpp 상단 주석 참고. x86_64 전용이다
# (init/initrun/CMakeLists.txt와 같은 이유).
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_netsrv INTERFACE)
  return()
endif()

add_executable(minicore_netsrv_elf
    main.cpp
)

target_include_directories(minicore_netsrv_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
)

target_link_libraries(minicore_netsrv_elf PRIVATE minicore_libk)

target_compile_options(minicore_netsrv_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_netsrv_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_netsrv_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
