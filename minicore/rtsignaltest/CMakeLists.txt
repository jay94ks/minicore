# minicore/rtsignaltest - PN-FD706AF6(SP-A7479F83 §6-B, RT 신호 큐잉)
# E2E 검증. signalfdtest와 동일한 out-of-tree add_subdirectory 관례.
add_executable(rtsignaltest main.cpp)

set_target_properties(rtsignaltest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(rtsignaltest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(rtsignaltest PRIVATE libmc libcpio libcenv)
