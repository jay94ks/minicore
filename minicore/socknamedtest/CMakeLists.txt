# minicore/socknamedtest - PN-F9CBF1A9(SP-231493CB §4-1 후속) E2E
# 검증 전용 클라이언트. ntfsdisktest 등과 동일한 out-of-tree
# add_subdirectory 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP
# kmain.cpp 훅으로만 자동 실행).
add_executable(socknamedtest main.cpp)

set_target_properties(socknamedtest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(socknamedtest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(socknamedtest PRIVATE libmc libcenv)
