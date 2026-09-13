# 커널 본체 (SP-8B6B8D25 §3.1) - ring 0에서 구동되는 유일한 구성 요소.
add_library(minicore_kernel OBJECT
    kmain.cpp
    serial.cpp
    idt.cpp
    page_frame_allocator.cpp
    paging.cpp
    acpi.cpp
    lapic.cpp
    ioapic.cpp
    timer.cpp
)

target_link_libraries(minicore_kernel PUBLIC libkenv minicore_libs_x86_64)
