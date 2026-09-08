# 커널·시스템 서버 공용 프리스탠딩 코어 라이브러리 (ADR-010).
# 현재는 헤더 디렉토리만 있는 골격 단계 — Result/Optional/Span/IntrusiveList 등이
# 채워지면 이 타깃을 통해 kernel/, servers/*에서 include한다.

add_library(minicore_libk INTERFACE)
target_include_directories(minicore_libk INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
