# minicore/granttest - DC-2B22FBF0(GrantSudoPermission 권한 검사) E2E
# 검증 전용 클라이언트. createusertest와 동일한 out-of-tree
# add_subdirectory 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP
# kmain.cpp 훅으로만 자동 실행).
add_executable(granttest main.cpp)

set_target_properties(granttest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(granttest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(granttest PRIVATE libmc libcenv)
