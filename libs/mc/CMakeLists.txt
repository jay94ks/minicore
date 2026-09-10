# mc(구 libmc) — minicore 네이티브 유저랜드 API, 순수 C+어셈블러
# (docs/design/foundations.md ADR-132/170, 저장소 배치는 ADR-199 —
# libs/mc). x86_64 전용(init/initrun/CMakeLists.txt와 같은 이유 —
# 다른 아키텍처의 syscall 트램폴린은 아직 없다). `#include <mc/...>`
# 경로 자체는 안 바뀌었다 — 내부 세그먼트 이름이 이미 `mc`였다.
#
# ADR-170이 확정한 범위: syscall 1:1 래퍼(mc/syscall.h, 헤더 전용)+
# VFS/FS 공통 프로토콜·콘솔·ps2 클라이언트+_start 진입점 글루.
# devmgr/cfgsrv 클라이언트는 아직 어떤 소비자도 없어 비워 둔다 —
# procsrv 클라이언트는 M32(real-libc-syscall-layer.md §M32)가 musl의
# syscall_shim.c를 첫 소비자로 만들며 추가했다(mc/procsrv_client.h).
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_libmc INTERFACE)
  return()
endif()

add_library(minicore_libmc STATIC
    src/ipc/vfs_client.c
    src/ipc/fs_client.c
    src/ipc/console_client.c
    src/ipc/ps2_client.c
    src/ipc/procsrv_client.c
    src/ipc/lifecycle_client.c
    src/ipc/cfgsrv_client.c
    src/ipc/pipesrv_client.c
    src/mem/heap.c
    src/x86_64/start.c
    src/freestanding_mem.c
)

target_include_directories(minicore_libmc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# freestanding_mem.c 자신의 이유(파일 상단 주석과 kernel/CMakeLists.txt
# 의 같은 처리) — 아니면 아래 루프가 다시 memcpy/memset 호출로
# 최적화돼 무한 재귀가 된다.
set_source_files_properties(src/freestanding_mem.c PROPERTIES
    COMPILE_OPTIONS "-fno-builtin"
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
