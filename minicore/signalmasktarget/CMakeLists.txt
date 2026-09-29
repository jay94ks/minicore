# minicore/signalmasktarget - signalmasktest(PN-A1A0B595 선행 작업,
# QU-28D7C7B1 답변(A) 검증)의 자식 프로세스 역할 전용 헬퍼. 자기
# 자신의 시그널 마스크는 절대 건드리지 않는다 - 부모가 스폰 전에
# 세팅한 마스크를 그대로 물려받았는지(SP-0666DB3C §4.6 상속)를
# 검증하는 게 이 바이너리의 존재 이유다.
add_executable(signalmasktarget main.cpp)

set_target_properties(signalmasktarget PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(signalmasktarget PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(signalmasktarget PRIVATE libmc libcenv)
