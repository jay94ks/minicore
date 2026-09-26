# minicore/sockclient - minicore/socktest(PN-CC0F4EAC)의 클라이언트
# 반쪽. dbgtarget/proctest와 동일한 이유의 out-of-tree add_subdirectory
# 관례(userland/ 빌드 트리 재사용) - **의도적으로 부팅 매니페스트에는
# 포함하지 않는다** - main.cpp 상단 주석 참고, socktest가 SpawnProcess
# syscall로만 스폰한다.
add_executable(sockclient main.cpp)

set_target_properties(sockclient PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(sockclient PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(sockclient PRIVATE libmc)
