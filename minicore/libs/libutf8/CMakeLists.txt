# UTF-8 인코딩/유니코드 유틸(SP-CCACB192 §4/§6) - kernel/userland 공용이라
# kernel:: 네임스페이스나 freestanding 전용 타입에 의존하지 않는다(libcpio
# 와 동일한 원칙, RM-23F4B687 §3의 minicore/libs/<name> 배치 규칙).
add_library(libutf8 OBJECT
    utf8.cpp
)

target_include_directories(libutf8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
