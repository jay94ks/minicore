# JSON 파서/직렬화(SP-CCACB192, SP-B071E628 §4) - kernel/userland
# 공용이라 kernel:: 네임스페이스나 freestanding 전용 타입에 의존하지
# 않는다(libcpio와 동일한 원칙, RM-23F4B687 §3의 minicore/libs/<name>
# 배치 규칙). `\uXXXX` 디코딩은 libutf8을 include하지만(json.cpp의
# `#include "libutf8/utf8.h"`) 별도 target_link_libraries는 필요 없다 -
# 이 저장소의 다른 OBJECT 라이브러리들과 마찬가지로 실제 심볼 연결은
# 최종 실행 파일(minicore.elf) 링크 시점에 이뤄진다(top-level
# CMakeLists.txt가 libutf8도 함께 링크).
add_library(libjson OBJECT
    json.cpp
)

target_include_directories(libjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
