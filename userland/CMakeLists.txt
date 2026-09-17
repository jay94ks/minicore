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

add_subdirectory(libs/libmc)
add_subdirectory(tests/userlandtest)

# minicore/init(SP-68182FBD §2.4) - userland/ 밖에 있지만 다른 유저
# 프로그램과 완전히 같은 실행 파일 형태(ET_EXEC, 0x400000, 비-PIE)로
# 빌드해야 해서 이 트리를 그대로 재사용(out-of-tree add_subdirectory,
# 바이너리 디렉터리를 명시해 소스 트리 밖에서도 안전).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/init ${CMAKE_CURRENT_BINARY_DIR}/minicore-init)

# devmgr(SP-9DD4F3EA §6, PN-BD9AAE2F) - init과 동일한 이유의 out-of-tree
# add_subdirectory.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/devmgr ${CMAKE_CURRENT_BINARY_DIR}/minicore-devmgr)

# pubreg(SP-B071E628, PN-185406F6) - init/devmgr과 동일한 이유의
# out-of-tree add_subdirectory.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/pubreg ${CMAKE_CURRENT_BINARY_DIR}/minicore-pubreg)

# fs(SP-8B6B8D25 §4-A, PN-452FF696) - init/devmgr/pubreg와 동일한
# 이유의 out-of-tree add_subdirectory.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/fs ${CMAKE_CURRENT_BINARY_DIR}/minicore-fs)
