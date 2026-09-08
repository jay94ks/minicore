# x86_64 GCC 크로스 툴체인 (ADR-020: 폴백)
# x86_64-elf-gcc / x86_64-elf-g++ 가 PATH에 있어야 한다.
set(MINICORE_TOOLCHAIN_ARCH x86_64)

set(CMAKE_C_COMPILER   x86_64-elf-gcc)
set(CMAKE_CXX_COMPILER x86_64-elf-g++)
set(CMAKE_ASM_COMPILER x86_64-elf-gcc)

include("${CMAKE_CURRENT_LIST_DIR}/common.cmake")
