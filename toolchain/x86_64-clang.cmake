# x86_64 Clang/LLVM 툴체인 (ADR-020: 기본 툴체인)
set(MINICORE_TOOLCHAIN_ARCH x86_64)

set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)

set(CMAKE_C_COMPILER_TARGET   x86_64-unknown-none-elf)
set(CMAKE_CXX_COMPILER_TARGET x86_64-unknown-none-elf)
set(CMAKE_ASM_COMPILER_TARGET x86_64-unknown-none-elf)

include("${CMAKE_CURRENT_LIST_DIR}/common.cmake")
