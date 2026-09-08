# aarch64 GCC 크로스 툴체인 (ADR-020: 폴백)
# aarch64-elf-gcc / aarch64-elf-g++ 가 PATH에 있어야 한다.
set(MINICORE_TOOLCHAIN_ARCH aarch64)

set(CMAKE_C_COMPILER   aarch64-elf-gcc)
set(CMAKE_CXX_COMPILER aarch64-elf-g++)
set(CMAKE_ASM_COMPILER aarch64-elf-gcc)

include("${CMAKE_CURRENT_LIST_DIR}/common.cmake")
