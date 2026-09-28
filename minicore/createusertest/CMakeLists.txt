# minicore/createusertest - DC-CC83F7BE(authmgr CreateUser 권한 검사,
# 커널 중개 방식) E2E 검증 전용 클라이언트. setuidtest/sudotest와
# 동일한 out-of-tree add_subdirectory 관례 - 부팅 매니페스트에는
# 포함하지 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
add_executable(createusertest main.cpp)

set_target_properties(createusertest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(createusertest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(createusertest PRIVATE libmc libcenv)
