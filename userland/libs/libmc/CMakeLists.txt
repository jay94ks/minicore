# libmc - Minicore 유저랜드 syscall 래퍼 라이브러리(PN-16CA347D 3번,
# userland/{apps,libs,tests} 체계의 첫 lib). 실제 syscall 진입 메커니즘
# (int 0x80/syscall 명령, ring3 진입)은 PN-124C105B가 아직 확정하지
# 않아, 지금은 endpoint 번호/호출 인터페이스만 고정하고 본문은 스텁으로
# 둔다(mc/syscall.cpp 참고).
add_library(libmc OBJECT
    syscall.cpp
)

# "libmc/syscall.h"처럼 라이브러리 이름을 포함한 경로로 include하게
# 하려고, libmc 디렉터리 자체가 아니라 그 부모(userland/libs)를 public
# include 경로로 노출한다(minicore/libs/libkenv와 같은 관례).
target_include_directories(libmc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
