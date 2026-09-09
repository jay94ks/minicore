# libmc — minicore 네이티브 유저랜드 API, 순수 C+어셈블러
# (docs/design/foundations.md ADR-132/170). x86_64 전용
# (init/initrun/CMakeLists.txt와 같은 이유 — 다른 아키텍처의
# syscall 트램폴린은 아직 없다).
#
# ADR-170이 확정한 이번 라운드의 범위: syscall 1:1 래퍼(mc/syscall.h,
# 헤더 전용)+VFS/FS 공통 프로토콜·콘솔·ps2 클라이언트+_start 진입점
# 글루뿐이다 — procsrv/devmgr/cfgsrv 클라이언트는 아직 어떤
# 소비자도 없어 비워 둔다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_libmc INTERFACE)
  return()
endif()

add_library(minicore_libmc STATIC
    src/ipc/vfs_client.c
    src/ipc/fs_client.c
    src/ipc/console_client.c
    src/ipc/ps2_client.c
    src/x86_64/start.c
)

target_include_directories(minicore_libmc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# init/initrun/CMakeLists.txt와 같은 이유(SSE/MMX 미사용, red zone
# 불필요 — 이 syscall ABI는 SSE 레지스터를 쓰지 않는다).
target_compile_options(minicore_libmc PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
