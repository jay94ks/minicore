# minicore/ntfsdisktest - PN-2A0981B7 항목2(NTFS 실제 Chmod 쓰기
# 경로, QU-9F8AD7A8 답변(A)) E2E 검증 전용 클라이언트.
# diskmetatest/fatdisktest/exfatdisktest와 동일한 out-of-tree
# add_subdirectory 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP
# kmain.cpp 훅으로만 자동 실행).
add_executable(ntfsdisktest main.cpp)

set_target_properties(ntfsdisktest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(ntfsdisktest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(ntfsdisktest PRIVATE libmc libcenv)
