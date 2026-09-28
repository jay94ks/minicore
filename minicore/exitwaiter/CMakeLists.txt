# minicore/exitwaiter - PN-5EDE3C96(항목1, exitCode 배선) 실측 검증
# 하네스. sockinherit과 동일한 이유로 out-of-tree add_subdirectory
# 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP kmain.cpp 훅으로만
# 자동 실행, main.cpp 상단 주석 참고).
add_executable(exitwaiter main.cpp)

set_target_properties(exitwaiter PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(exitwaiter PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(exitwaiter PRIVATE libmc libcpio)
