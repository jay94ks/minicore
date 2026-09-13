cmake_minimum_required(VERSION 3.20)
project(minicore CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 서브디렉터리 전부에 공통 적용 - 각 모듈 CMakeLists.txt에서 따로
# 반복하지 않는다.
add_compile_options(-Wall -Wextra)

set(MINICORE_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/minicore/arch/x86_64/linker.ld)

# 모듈별 CMakeLists.txt (SP-8B6B8D25 §3 디렉터리 배치와 1:1 대응) -
# 새 모듈(devmgr/fs/net/tty 등, 전부 유저랜드)이 생기면 여기 목록에
# 추가한다. 전부 OBJECT 라이브러리라 실제 아카이빙/심볼가지치기 없이
# 오브젝트가 그대로 최종 실행 파일에 들어간다.
add_subdirectory(minicore/libs/libkenv)
add_subdirectory(minicore/libs/x86_64)
add_subdirectory(minicore/arch/x86_64)
add_subdirectory(minicore/kernel)

add_executable(minicore.elf)
target_link_libraries(minicore.elf PRIVATE
    minicore_arch_x86_64
    minicore_kernel
    libkenv
    minicore_libs_x86_64
)

set_target_properties(minicore.elf PROPERTIES
    LINK_DEPENDS ${MINICORE_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(minicore.elf PRIVATE -T ${MINICORE_LINKER_SCRIPT})
