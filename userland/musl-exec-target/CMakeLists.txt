# musl-hello가 fork()+execve()로 실행 이미지를 바꿔 넣을 "다른"
# 바이너리(real-libc-syscall-layer.md §M32). userland/musl-hello/
# CMakeLists.txt와 완전히 같은 이유·설정.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_musl_exec_target INTERFACE)
  return()
endif()

add_executable(minicore_musl_exec_target_elf
    main.c
)

target_link_libraries(minicore_musl_exec_target_elf PRIVATE minicore_libc minicore_libmc)

target_compile_options(minicore_musl_exec_target_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_C_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_musl_exec_target_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_musl_exec_target_elf PROPERTIES
    LINKER_LANGUAGE C
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)

# 이 바이너리는 initrun이 부팅 시 스폰하는 서비스가 아니다(servers/
# CMakeLists.txt --service=에 없음) — musl-hello가 런타임에 진짜
# execve()로 실행하는 대상일 뿐이다. 그 바이트는 procsrv가 VFS
# (memfs)에 미리 써 둬야 하므로, procsrv/CMakeLists.txt가
# tools/bin2c.py로 이 타깃의 $<TARGET_FILE:...>을 C++ 헤더로 구워
# procsrv 자신의 소스에 심는다.
