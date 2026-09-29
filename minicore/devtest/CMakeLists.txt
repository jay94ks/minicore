# minicore/devtest - SP-23880DC6(DeviceRegistry) E2E 검증 전용
# 클라이언트. socknamedtest 등과 동일한 out-of-tree add_subdirectory
# 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP kmain.cpp 훅으로만
# 자동 실행).
add_executable(devtest main.cpp)

set_target_properties(devtest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(devtest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(devtest PRIVATE libmc libcenv)
