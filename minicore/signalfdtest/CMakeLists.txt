# minicore/signalfdtest - PN-A1A0B595(SP-A7479F83 §2/§3/§4, signalfd)
# E2E 검증. setuidspawntest/signalmasktest와 동일한 out-of-tree
# add_subdirectory 관례.
add_executable(signalfdtest main.cpp)

set_target_properties(signalfdtest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(signalfdtest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(signalfdtest PRIVATE libmc libcpio libcenv)
