# minicore/exittest - PN-5EDE3C96(항목1, exitCode 배선) 실측 검증
# 전용 자식 프로그램. setuidtest/sudotest 등과 동일한 out-of-tree
# add_subdirectory 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP
# kmain.cpp 훅은 exitwaiter만 스폰하고, 이 프로그램은 exitwaiter가
# 실제 SpawnProcess로 스폰한다 - main.cpp 상단 주석 참고).
add_executable(exittest main.cpp)

set_target_properties(exittest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(exittest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(exittest PRIVATE libmc libcenv)
