# minicore/signalfdchild - signalfdtest(PN-A1A0B595, SP-A7479F83 §2/§3/§4
# E2E 검증)의 자식 프로세스 역할 전용 헬퍼.
add_executable(signalfdchild main.cpp)

set_target_properties(signalfdchild PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(signalfdchild PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(signalfdchild PRIVATE libmc libcenv)
