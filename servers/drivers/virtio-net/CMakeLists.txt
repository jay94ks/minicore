# virtio-net 드라이버 (docs/plan/general-purpose-completion.md §M25).
# servers/drivers/virtio-blk/CMakeLists.txt와 완전히 같은 이유로
# x86_64 전용이다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_drv_virtio_net INTERFACE)
  return()
endif()

add_executable(minicore_drv_virtio_net_elf
    main.cpp
)

target_include_directories(minicore_drv_virtio_net_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/mc/include
)

target_link_libraries(minicore_drv_virtio_net_elf PRIVATE minicore_libk minicore_libmc)

target_compile_options(minicore_drv_virtio_net_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_drv_virtio_net_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_drv_virtio_net_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
