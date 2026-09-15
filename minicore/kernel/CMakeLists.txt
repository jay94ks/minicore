# 커널 본체 (SP-8B6B8D25 §3.1) - ring 0에서 구동되는 유일한 구성 요소.
add_library(minicore_kernel OBJECT
    kmain.cpp
    multiboot2.cpp
    serial.cpp
    gdt.cpp
    idt.cpp
    page_frame_allocator.cpp
    paging.cpp
    acpi.cpp
    lapic.cpp
    ioapic.cpp
    hpet.cpp
    pci.cpp
    smp.cpp
    timer.cpp
    task.cpp
    scheduler.cpp
    async_task.cpp
    wait_queue.cpp
    syscall.cpp
    named_object.cpp
    channel.cpp
    process.cpp
)

target_link_libraries(minicore_kernel PUBLIC libkenv libelf minicore_libs_x86_64)

target_compile_definitions(minicore_kernel PRIVATE
    MINICORE_TASK_STACK_GUARD_PAGE=$<BOOL:${MINICORE_TASK_STACK_GUARD_PAGE}>
)
