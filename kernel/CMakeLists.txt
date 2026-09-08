# 커널 (직접 구현, ADR-006). 골격 단계 — 실제 소스는 별도 계획에서 추가한다.
#
# 채워질 예정:
#   core/ipc, core/sched, core/mm, core/object  (arch 독립, ADR-004/011/012/013/014/015/016)
#   arch/${MINICORE_ARCH}                        (ADR-002/009/017; arch 의존 코드는 여기에만)
#
# 소스가 추가되면 INTERFACE 대신 STATIC/OBJECT 라이브러리로 전환하고
# 링커 스크립트(kernel/arch/${MINICORE_ARCH}/linker.ld)를 연결한다.

add_library(minicore_kernel INTERFACE)
target_include_directories(minicore_kernel INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(minicore_kernel INTERFACE minicore_libk)
