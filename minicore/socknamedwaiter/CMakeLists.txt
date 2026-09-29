# minicore/socknamedwaiter - PN-F9CBF1A9 검증 하네스. exitwaiter와
# 동일한 이유로 out-of-tree add_subdirectory 관례 - 부팅 매니페스트에는
# 포함하지 않는다(TEMP kmain.cpp 훅으로만 자동 실행, main.cpp 상단
# 주석 참고).
add_executable(socknamedwaiter main.cpp)

set_target_properties(socknamedwaiter PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(socknamedwaiter PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(socknamedwaiter PRIVATE libmc libcpio)
