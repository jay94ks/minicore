# minicore/authtest - authmgr(PN-24A2B6F5) libkvdb 연동(CreateUser/
# LookupByUid) E2E 검증 전용 클라이언트. dbgtarget/sockclient와 동일한
# out-of-tree add_subdirectory 관례 - 의도적으로 부팅 매니페스트에는
# 포함하지 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
add_executable(authtest main.cpp)

set_target_properties(authtest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(authtest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(authtest PRIVATE libmc libcenv)
