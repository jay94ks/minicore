# minicore/sudotest - DC-34764C25 항목1(sudo/su 화이트리스트) E2E
# 검증 전용 클라이언트. setuidtest와 동일한 out-of-tree
# add_subdirectory 관례 - 의도적으로 부팅 매니페스트에는 포함하지
# 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
add_executable(sudotest main.cpp)

set_target_properties(sudotest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(sudotest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(sudotest PRIVATE libmc libcenv)
