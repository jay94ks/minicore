# CPIO 아카이브 파서(QU-9DCDCE3E) - kernel/userland 공용이라 kernel::
# 네임스페이스나 freestanding 전용 타입에 의존하지 않는다(표준 C++
# 타입만 사용, RM-23F4B687 §3의 minicore/libs/<name> 배치 규칙).
add_library(libcpio OBJECT
    cpio.cpp
)

target_include_directories(libcpio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
