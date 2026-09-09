# 프로세스 서버: fork/exec/signal/wait/pid, fd 진실 공급원 (ADR-016).
# M12(system-servers-bringup.md) 골격 — main.cpp 상단 주석 참고.
# init/initrun/CMakeLists.txt와 같은 이유로 x86_64 전용이다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_procsrv INTERFACE)
  return()
endif()

add_executable(minicore_procsrv_elf
    main.cpp
)

target_include_directories(minicore_procsrv_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
)

target_link_libraries(minicore_procsrv_elf PRIVATE minicore_libk)

# init/initrun/CMakeLists.txt와 같은 이유(SSE/MMX 미사용, red zone 불필요).
target_compile_options(minicore_procsrv_elf PRIVATE
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

target_link_options(minicore_procsrv_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_procsrv_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)

# 부트 디스크(bootdisk.img) 조립은 M13부터 여러 서비스(procsrv/vfs/
# memfs)를 함께 담아야 해서 servers/CMakeLists.txt로 옮겼다 —
# 거기서만 이 셋을 모두 참조할 수 있다.
