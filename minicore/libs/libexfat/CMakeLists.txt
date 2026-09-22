# libexfat - exFAT 온디스크 포맷 읽기/쓰기 라이브러리(1차 증분,
# SP-F1987EF8, PN-C93A4E9E) - libext4/libvfat/libswapfs와 동일한 사정
# (fs가 SP-43331889로 Process 없는 순수 커널 KernelThread에 흡수된
# 뒤라 유일한 소비자가 커널 자신뿐)으로 커널 전용 - libelf/libcpio처럼
# 별도 유저랜드 컴파일 대상을 남겨 두지 않는다(실사용처가 생기면 그때
# 확장, RM-23F4B687 §4).
add_library(libexfat OBJECT
    exfat.cpp
    exfat_driver.cpp
)

# libext4/libvfat/libswapfs와 동일한 이유로 minicore/kernel도
# PUBLIC이어야 한다 - exfat.cpp 자신이 `#include "block_device.h"`
# (커널 헤더)를 하므로, libexfat을 링크하는 다른 타깃(minicore_kernel)
# 이 그 transitive include를 풀 수 있어야 한다.
target_include_directories(libexfat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/minicore/kernel)
