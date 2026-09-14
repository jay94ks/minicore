# Minicore 유저랜드 빌드 루트(SP-8B6B8D25 §5-A, QU-FF3F0CAA 답변 확정)
# - 커널과 완전히 별도의 CMake 빌드 트리다(userland/cmake/
# toolchain-userland-x86_64.cmake로 구성):
#
#   cmake -S userland -B build-userland -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=userland/cmake/toolchain-userland-x86_64.cmake
#   cmake --build build-userland
#
# 디렉터리 체계(설계자 지시, 2026-09-14): apps/ libs/ tests/ 하위에
# 프로젝트 이름별 폴더 - 새 하위 프로젝트를 만들 때마다 add_subdirectory
# 를 추가하고 RM-7C249618("Minicore 라이브러리 목록")도 갱신한다.
cmake_minimum_required(VERSION 3.20)
project(minicore-userland CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

set(MINICORE_USERLAND_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/linker-userland-x86_64.ld)

add_subdirectory(tests/userlandtest)
