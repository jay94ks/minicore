# svcmgr 컨트롤 프로토콜 검증용 최소 테스트 프로그램
# (user-service-manager.md §M42). userland/svcmgr-demo-unit/CMakeLists.txt
# 와 같은 설정(libmc만 링크, 순수 네이티브) — 다만 initrun이 부팅
# 시 직접 스폰하는 정식 서비스다(servers/CMakeLists.txt --service=).
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_svcmgr_ctl_test INTERFACE)
  return()
endif()

add_executable(minicore_svcmgr_ctl_test_elf
    main.c
)

target_link_libraries(minicore_svcmgr_ctl_test_elf PRIVATE minicore_libmc)

target_compile_options(minicore_svcmgr_ctl_test_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_C_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_svcmgr_ctl_test_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_svcmgr_ctl_test_elf PROPERTIES
    LINKER_LANGUAGE C
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
