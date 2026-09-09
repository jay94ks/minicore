# 메모리 파일시스템: ADR-008 수직 슬라이스의 최초 구현 대상.
# M13(system-servers-bringup.md) 최소 버전 — main.cpp 상단 주석 참고.
# init/initrun/CMakeLists.txt와 같은 이유로 x86_64 전용이다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_memfs INTERFACE)
  return()
endif()

add_executable(minicore_memfs_elf
    main.cpp
)

target_include_directories(minicore_memfs_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
)

target_link_libraries(minicore_memfs_elf PRIVATE minicore_libk)

# init/initrun/CMakeLists.txt와 같은 이유(SSE/MMX 미사용, red zone 불필요).
target_compile_options(minicore_memfs_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

# init/initrun/CMakeLists.txt와 같은 이유(Windows 호스트 clang++ 링크
# 드라이버 우회) — ld.lld를 직접 호출한다.
set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_memfs_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_memfs_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
