# VGA 텍스트 콘솔 드라이버(M17, boot-and-drivers.md ADR-164). main.cpp
# 상단 주석 참고. init/initrun/CMakeLists.txt와 같은 이유로 x86_64
# 전용이다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_drv_console INTERFACE)
  return()
endif()

add_executable(minicore_drv_console_elf
    main.cpp
)

target_include_directories(minicore_drv_console_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
)

target_link_libraries(minicore_drv_console_elf PRIVATE minicore_libk)

target_compile_options(minicore_drv_console_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_drv_console_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_drv_console_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
