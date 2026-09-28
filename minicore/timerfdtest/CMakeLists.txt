# minicore/timerfdtest - PN-96265AE4/PN-0F56DE4B(timerfd) E2E 검증
# 전용 클라이언트. setuidtest/lrutest 등과 동일한 out-of-tree
# add_subdirectory 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP
# kmain.cpp 훅으로만 자동 실행).
add_executable(timerfdtest main.cpp)

set_target_properties(timerfdtest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(timerfdtest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(timerfdtest PRIVATE libmc libcenv)
