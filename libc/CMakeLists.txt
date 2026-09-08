# POSIX 계층 (ADR-005/008). 채택할 libc(third_party/<libc>, ADR-022)가 아직
# 확정되지 않아, 현재는 minicore 전용 syscall/서버 연결 글루 헤더만 노출한다.

add_library(minicore_libc_glue INTERFACE)
target_include_directories(minicore_libc_glue INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/sysdeps/minicore
)

# TODO: libc 채택 확정 후 third_party/<libc>를 add_subdirectory 또는
#       ExternalProject_Add로 연결하고, 패치 적용(tools/apply-patches.*)을 선행한다.
