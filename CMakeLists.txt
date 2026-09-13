cmake_minimum_required(VERSION 3.20)
project(minicore CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MINICORE_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/minicore/arch/x86_64/linker.ld)

add_executable(minicore.elf
    minicore/arch/x86_64/boot.S
    minicore/arch/x86_64/isr.S
    minicore/kernel/kmain.cpp
    minicore/kernel/serial.cpp
    minicore/kernel/idt.cpp
    minicore/kernel/page_frame_allocator.cpp
    minicore/kernel/paging.cpp
)

target_compile_options(minicore.elf PRIVATE -Wall -Wextra)

set_target_properties(minicore.elf PROPERTIES
    LINK_DEPENDS ${MINICORE_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(minicore.elf PRIVATE -T ${MINICORE_LINKER_SCRIPT})
