# minicore/exfatdisktest - PN-2A0981B7 항목1(exFAT 실제 Chmod 쓰기
# 경로) E2E 검증 전용 클라이언트. diskmetatest/fatdisktest와 동일한
# out-of-tree add_subdirectory 관례 - 부팅 매니페스트에는 포함하지
# 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
add_executable(exfatdisktest main.cpp)

set_target_properties(exfatdisktest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(exfatdisktest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(exfatdisktest PRIVATE libmc libcenv)
