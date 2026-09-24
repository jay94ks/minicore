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
add_subdirectory(libs/libcenv)
add_subdirectory(libs/libcpio)

# arch/x86_64(SP-8B6B8D25 §3.0, RM-23F4B687 §3 갱신, PN-E612E714) -
# io_port.h/msr.h는 애초에 유저/커널 공용을 전제로 설계됐다
# (QU-4606360C) - init/pubreg 등과 동일한 이유의 out-of-tree
# add_subdirectory(커널 빌드 트리와 완전히 별개 CMake 프로젝트라
# 양쪽에 각각 add_subdirectory 필요). 이 타깃(OBJECT 라이브러리,
# isr.S/context_switch.S/syscall_entry.S는 커널 전용) 자체를 지금
# 당장 어느 유저랜드 실행 파일이 링크하는 건 아니다 - 헤더 include
# 경로를 노출해 두는 게 목적이고, 실제 컴파일 확인은 아래
# userlandtest에서 별도로 한다(이 OBJECT 타깃을 링크하면 커널 전용
# .S 파일들의 심볼까지 끌려와 불필요한 링크 문제가 생길 수 있어
# 피한다).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/arch/x86_64 ${CMAKE_CURRENT_BINARY_DIR}/minicore-arch-x86_64)

add_subdirectory(tests/userlandtest)

# minicore/init(SP-68182FBD §2.4) - userland/ 밖에 있지만 다른 유저
# 프로그램과 완전히 같은 실행 파일 형태(ET_EXEC, 0x400000, 비-PIE)로
# 빌드해야 해서 이 트리를 그대로 재사용(out-of-tree add_subdirectory,
# 바이너리 디렉터리를 명시해 소스 트리 밖에서도 안전).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/init ${CMAKE_CURRENT_BINARY_DIR}/minicore-init)

# devmgr(SP-9DD4F3EA §6, PN-BD9AAE2F) - **[제외, 2026-09-20,
# SP-43331889/QU-23B339AB/QU-ECEE5990] 더 이상 유저랜드 ELF가 아니다**,
# Process 없는 순수 커널 KernelThread로 완전 흡수돼 이 유저랜드 빌드
# 트리에서 뺐다. **[정리, 2026-09-21, 설계자 지시, PN-D6A05E78]**
# minicore/devmgr 디렉터리 자체가 사라졌다 - 소스는
# minicore/kernel/devmgr.cpp로 옮겨 top-level CMakeLists.txt가 커널
# 빌드 트리(minicore_kernel 소스 목록)에 직접 넣는다.

# pubreg(SP-B071E628, PN-185406F6) - init/devmgr과 동일한 이유의
# out-of-tree add_subdirectory.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/pubreg ${CMAKE_CURRENT_BINARY_DIR}/minicore-pubreg)

# libkvdb(SP-30FCC8AE §1-D, PN-24A2B6F5 §1-C/D 항목1) - authmgr(유저
# 랜드 프로세스)의 의존성이라 이 유저랜드 빌드 트리에도 필요하다.
# 커널 빌드 트리(top-level CMakeLists.txt)와는 완전히 별개 CMake
# 프로젝트라 add_subdirectory를 양쪽에 각각 둬야 한다(순수 헤더
# INTERFACE 라이브러리라 이중 컴파일 걱정은 없음) - init/pubreg와
# 동일한 out-of-tree 관례.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/libs/libkvdb ${CMAKE_CURRENT_BINARY_DIR}/minicore-libkvdb)

# authmgr(SP-8B6B8D25 §3.1 항목8, PN-24A2B6F5/PN-CFEAEF40) -
# init/pubreg와 동일한 이유의 out-of-tree add_subdirectory.
# [갱신, 2026-09-23, PN-24A2B6F5] libkvdb 연동 시작 - 스캐폴딩만
# 이던 v1에서 확장.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/authmgr ${CMAKE_CURRENT_BINARY_DIR}/minicore-authmgr)

# fs(SP-8B6B8D25 §4-A, PN-452FF696) - **[제외, 2026-09-20,
# SP-43331889/QU-5FC58B06] 더 이상 유저랜드 ELF가 아니다**, devmgr과
# 동일한 이유로 Process 없는 순수 커널 KernelThread로 완전 흡수돼
# 이 유저랜드 빌드 트리에서 뺐다. **[정리, 2026-09-21, 설계자 지시,
# PN-D6A05E78]** minicore/fs 디렉터리 자체가 사라졌다 - 소스(main.cpp,
# ahci.h/.cpp, block_device.h)는 전부 minicore/kernel/로 옮겨
# top-level CMakeLists.txt가 커널 빌드 트리에 직접 넣는다.

# dbgtarget(PN-87D6B615 "남은 범위" 2번, 디버그 세션 E2E 전용 디버기) -
# init/devmgr/pubreg/fs와 동일한 이유의 out-of-tree add_subdirectory.
# **부팅 매니페스트(scripts/build-initrd.sh)에는 의도적으로 포함하지
# 않는다** - minicore/dbgtarget/main.cpp 상단 주석 참고.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/dbgtarget ${CMAKE_CURRENT_BINARY_DIR}/minicore-dbgtarget)

# proctest(PN-012D6310, procfs pid 열람 실측 검증 전용) - dbgtarget과
# 동일한 이유의 out-of-tree add_subdirectory. **부팅 매니페스트
# (scripts/build-initrd.sh의 고정 스폰 목록)에는 의도적으로 포함하지
# 않는다** - minicore/proctest/main.cpp 상단 주석 참고.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/proctest ${CMAKE_CURRENT_BINARY_DIR}/minicore-proctest)
