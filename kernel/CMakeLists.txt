# 커널 (직접 구현, ADR-006).
#
#   core/          arch 독립 코어 (ADR-004/011/012/013/014/015/016).
#                  M1~M4 현재는 klog(debug-console.md), boot_info 덤프
#                  (boot.md §3), panic_hook(ADR-067), mm(물리 페이지
#                  할당자+슬랩 힙, memory.md), object(핸들 테이블+프록시
#                  트리, objects.md)가 있다 — ipc/sched는 이후
#                  마일스톤(M5~M6)에서 채워진다.
#   arch/${MINICORE_ARCH}/  arch 의존 코드 (ADR-002/009/017) — 부트 스텁,
#                  실제 부팅 가능한 커널 실행파일은 여기서 링크한다.

add_library(minicore_kernel INTERFACE)
target_include_directories(minicore_kernel INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(minicore_kernel INTERFACE minicore_libk)

# arch 독립 커널 코어 소스. kernel/core는 arch 헤더를 include하지 않는다
# (ADR-002 HAL 경계) — klog::putc()/arch_irq_save 등은 arch 쪽이
# 구현해 링크 시점에 채워진다.
add_library(minicore_kernel_core OBJECT
    core/klog.cpp
    core/boot_info_dump.cpp
    core/freestanding_mem.cpp
    core/panic.cpp
    core/mm/page_allocator.cpp
    core/mm/slab.cpp
    core/object/handle_table.cpp
)
target_link_libraries(minicore_kernel_core PUBLIC minicore_kernel)

# core/ 하위 헤더(mm/page_allocator.hpp 등)를 arch 쪽 소스(예:
# boot_info_x86_64.cpp)에서 <mm/xxx.hpp> 형태로 참조할 수 있도록
# PUBLIC으로 노출한다.
target_include_directories(minicore_kernel_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)

# kernel/core/mm이 <arch_mm_defs.hpp>(제네릭 이름)로 참조하는 arch별
# physmap 상수 헤더 — kernel/core 자신은 이게 x86_64용인지 모른다
# (ADR-002 HAL 경계). minicore_kernel_core 자신의 컴파일에만 필요하므로
# PRIVATE.
target_include_directories(minicore_kernel_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/${MINICORE_ARCH}
)

# -fno-builtin: freestanding_mem.cpp의 memcpy/memset/... 구현이 컴파일러의
# "이 루프는 memcpy 호출로 바꿔치기하면 더 빠르다" 최적화에 걸려 자기
# 자신을 호출하는 무한 재귀가 되는 것을 막는다(잘 알려진 함정).
set_source_files_properties(core/freestanding_mem.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-builtin"
)

add_subdirectory(arch/${MINICORE_ARCH})
