# 유저랜드 툴체인/링커 스크립트 검증용 최소 실행 파일(main.cpp 참고).
add_executable(userlandtest main.cpp)

set_target_properties(userlandtest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(userlandtest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
