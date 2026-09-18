# Minicore UEFI 직접 부팅 진입점 - PN-7FBF255A(BIOS/UEFI 직접 부팅
# 경로 구현) 체크리스트 1-3번. 커널(cmake/toolchain-x86_64.cmake,
# x86_64-elf 크로스 GCC/Binutils, ELF 출력)/유저랜드(userland/cmake/
# toolchain-userland-x86_64.cmake, x86_64-unknown-none-elf, ELF 출력)
# 와 완전히 다른 세 번째 독립 빌드 트리다 - UEFI 애플리케이션은
# PE32+(COFF) 형식이어야만 펌웨어가 로드할 수 있어(ELF가 아님) 별도
# 툴체인(clang -target x86_64-unknown-windows + lld -flavor link)이
# 필요하다(PN-7FBF255A "실행 가능성 조사"/"체크리스트 1번 실측 검증"
# 절 참고 - clang+lld COFF 직접 경로가 실제로 OVMF에서 부팅됨을
# 실측 확인).
#
# 빌드:
#   cmake -S minicore/boot-uefi -B build-uefi -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=minicore/boot-uefi/cmake/toolchain-uefi-x86_64.cmake
#   cmake --build build-uefi
cmake_minimum_required(VERSION 3.20)
project(minicore-boot-uefi CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

add_executable(bootx64 main.cpp)
target_include_directories(bootx64 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# UEFI 애플리케이션 서브시스템 + 진입점 - PE32+ EFI_STATUS EFIAPI
# efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE*) 시그니처(MS x64 호출 규약,
# clang이 x86_64-unknown-windows 타깃에서 자동으로 맞춰 준다).
target_link_options(bootx64 PRIVATE
    -Wl,-subsystem:efi_application
    -Wl,-entry:efi_main
)

set_target_properties(bootx64 PROPERTIES SUFFIX ".efi")
