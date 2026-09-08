# 커널·시스템 서버 공용 프리스탠딩 코어 라이브러리 (ADR-010, ADR-066).
# 헤더 전용(header-only) — result/optional/span/intrusive_list/atomic/
# spinlock/ticket_lock/mcs_lock/irq_safe/panic 전부 템플릿·인라인이라
# 별도 .cpp가 없다. kernel/, servers/*에서 <libk/xxx.hpp>로 include한다.
# 호스트 네이티브 단위 테스트는 libk/tests/(ADR-077, 이 크로스 빌드
# 트리와 독립된 별도 CMake 프로젝트)가 담당한다.

add_library(minicore_libk INTERFACE)
target_include_directories(minicore_libk INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
