# 유저랜드 툴체인/링커 스크립트 검증용 최소 실행 파일(main.cpp 참고).
add_executable(userlandtest main.cpp)

# PN-E612E714 체크리스트 7번 - x86_64/io_port.h가 유저랜드 툴체인에서도
# 실제로 컴파일되는지 확인(헤더만 include, minicore_arch_x86_64
# OBJECT 타깃 자체는 링크하지 않는다 - 상위 CMakeLists.txt 주석
# 참고, 커널 전용 .S 심볼이 불필요하게 끌려오는 걸 피하기 위함).
# io_port.h가 "libkenv/types.h"(uint8_t 등 별칭)를 include하므로
# libkenv 헤더 경로도 함께 노출한다(libkenv OBJECT 타깃 자체는
# 링크 안 함 - 순수 타입 별칭 헤더라 링크 불필요).
target_include_directories(userlandtest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../minicore/arch
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../minicore/libs
)

set_target_properties(userlandtest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(userlandtest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
