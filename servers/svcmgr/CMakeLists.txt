# 유저 서비스 관리자 데몬(user-service-manager.md §M40,
# docs/design/boot-and-drivers.md ADR-192/ADR-196). servers/procsrv/
# CMakeLists.txt의 musl-exec-target 임베딩과 완전히 같은 이유·패턴
# 으로 userland/svcmgr-demo-unit의 바이트를 컴파일 시점 데이터로
# 심는다 — svcmgr는 그 유닛을 VFS 없이 곧바로 process_spawn한다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_svcmgr INTERFACE)
  return()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(SVCMGR_DEMO_UNIT_BLOB_H ${CMAKE_CURRENT_BINARY_DIR}/generated/svcmgr_demo_unit_blob.h)
add_custom_command(
    OUTPUT ${SVCMGR_DEMO_UNIT_BLOB_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bin2c.py
            $<TARGET_FILE:minicore_svcmgr_demo_unit_elf>
            ${SVCMGR_DEMO_UNIT_BLOB_H}
            g_svcmgr_demo_unit_elf
    DEPENDS minicore_svcmgr_demo_unit_elf
            ${CMAKE_SOURCE_DIR}/tools/bin2c.py
    COMMENT "Generating svcmgr-demo-unit ELF blob header (tools/bin2c.py)"
    VERBATIM
)
add_custom_target(minicore_svcmgr_demo_unit_blob DEPENDS ${SVCMGR_DEMO_UNIT_BLOB_H})

add_executable(minicore_svcmgr_elf
    main.cpp
)

add_dependencies(minicore_svcmgr_elf minicore_svcmgr_demo_unit_blob)

target_include_directories(minicore_svcmgr_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/mc/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(minicore_svcmgr_elf PRIVATE minicore_libk minicore_libmc)

target_compile_options(minicore_svcmgr_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_svcmgr_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_svcmgr_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
