# minicore/ext4rwtest - PN-2A0981B7 항목5(QU-1E7DFB8C 답변(A), ext4
# 실제 쓰기 가능 마운트 E2E 검증) 전용 클라이언트. diskmetatest와
# 동일한 out-of-tree add_subdirectory 관례 - 부팅 매니페스트에는
# 포함하지 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
add_executable(ext4rwtest main.cpp)

set_target_properties(ext4rwtest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(ext4rwtest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(ext4rwtest PRIVATE libmc libcenv)
