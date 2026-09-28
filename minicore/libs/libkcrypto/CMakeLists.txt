# 암호학적 해시/암호화(SP-CC1CF30E §8) - kernel/userland 공용이라
# kernel:: 네임스페이스나 freestanding 전용 타입에 의존하지 않는다
# (libutf8/libcpio/libelf와 동일한 원칙, RM-23F4B687 §3의
# minicore/libs/<name> 배치 규칙).
add_library(libkcrypto OBJECT
    sha256.cpp
)

target_include_directories(libkcrypto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
