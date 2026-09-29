# minicore/signalmasktest - PN-A1A0B595 선행 작업(QU-28D7C7B1 답변(A),
# 프로세스 시그널 마스크) E2E 검증. setuidspawntest/exitwaiter와 동일한
# out-of-tree add_subdirectory 관례.
add_executable(signalmasktest main.cpp)

set_target_properties(signalmasktest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(signalmasktest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(signalmasktest PRIVATE libmc libcpio libcenv)
