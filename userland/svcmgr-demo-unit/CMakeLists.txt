# svcmgr가 스폰할 하드코딩된 데모 유닛 하나(user-service-manager.md
# §M40). userland/musl-exec-target/CMakeLists.txt와 같은 이유·설정
# (svcmgr 자신의 컴파일 시점 데이터로 tools/bin2c.py가 심는다 —
# servers/svcmgr/CMakeLists.txt 참고) — 다만 musl 없이 libmc만
# 링크한다(ADR-196 §결정1과 같은 정신, svcmgr 자신도 순수 네이티브).
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_svcmgr_demo_unit INTERFACE)
  return()
endif()

add_executable(minicore_svcmgr_demo_unit_elf
    main.c
)

target_link_libraries(minicore_svcmgr_demo_unit_elf PRIVATE minicore_libmc)

target_compile_options(minicore_svcmgr_demo_unit_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_C_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_svcmgr_demo_unit_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_svcmgr_demo_unit_elf PROPERTIES
    LINKER_LANGUAGE C
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)

# musl-exec-target과 같은 이유 — initrun이 부팅 시 스폰하는 서비스가
# 아니다(servers/CMakeLists.txt --service=에 없음). svcmgr가 자신의
# 컴파일 시점 데이터로 심어(tools/bin2c.py, servers/svcmgr/CMakeLists.txt)
# 곧바로 process_spawn한다 — VFS를 거치지 않는다(musl-exec-target과
# 달리 이 유닛은 execve()의 대상이 아니라 svcmgr가 직접 spawn하므로
# memfs에 미리 써 둘 필요가 없다).
