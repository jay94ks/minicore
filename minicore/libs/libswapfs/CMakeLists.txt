# libswapfs - Linux swap-space v2 온디스크 포맷 + SwapfsBackend
# (SP-D02C4A73, PN-6D9A5DAE) - fs가 SP-43331889로 Process 없는 순수
# 커널 KernelThread에 완전히 흡수된 뒤라 유일한 소비자가 커널 자신뿐
# 이다(block_device.h/swap_backend.h와 동일한 사정) - libelf/libcpio
# 처럼 별도 유저랜드 컴파일 대상을 남겨 두지 않는다(실사용처가 생기면
# 그때 확장, RM-23F4B687 §4).
add_library(libswapfs OBJECT
    swapfs.cpp
    swapfs_io.cpp
)

# [갱신, 2026-09-22] minicore/kernel도 PUBLIC이어야 한다 - swapfs.h
# 자신이 `#include "swap_backend.h"`(커널 헤더)를 하므로, libswapfs를
# 링크하는 다른 타깃(minicore_kernel)이 swapfs.h를 include할 때도
# 그 transitive include가 풀려야 한다(libelf의 MINICORE_LIBELF_KERNEL
# 매크로를 PUBLIC으로 두는 것과 같은 이유).
target_include_directories(libswapfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/minicore/kernel)
