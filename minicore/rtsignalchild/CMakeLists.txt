# minicore/rtsignalchild - rtsignaltest(PN-FD706AF6, SP-A7479F83 §6-B
# RT 신호 큐잉 E2E 검증)의 자식 프로세스 역할 전용 헬퍼.
add_executable(rtsignalchild main.cpp)

set_target_properties(rtsignalchild PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(rtsignalchild PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(rtsignalchild PRIVATE libmc libcenv)
