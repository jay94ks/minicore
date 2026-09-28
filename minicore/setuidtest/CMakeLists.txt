# minicore/setuidtest - DC-90A66932(A) 커널 전용 authmgr Channel
# 클라이언트 배선(kSetuidOnExecImpl, authmgr_client.h/.cpp) E2E 검증
# 전용 클라이언트. authtest와 동일한 out-of-tree add_subdirectory
# 관례 - 의도적으로 부팅 매니페스트에는 포함하지 않는다(TEMP kmain.cpp
# 훅으로만 자동 실행).
add_executable(setuidtest main.cpp)

set_target_properties(setuidtest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(setuidtest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(setuidtest PRIVATE libmc libcenv)
