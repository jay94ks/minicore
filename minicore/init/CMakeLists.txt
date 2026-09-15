# minicore/init - initrd가 하드코딩으로 최초 구동하는 유일한 유저
# 프로그램(SP-68182FBD §2.4, QU-FF3F0CAA 답변 확정 - "init 앱은 유저
# 영역의 일반 유저 앱과 커널 앱의 중간 지점"이라 예외적으로
# userland/ 밖 이 자리에 둔다). 다만 실행 파일 형태(ET_EXEC, 코드
# 베이스 0x400000, 비-PIE)는 다른 유저 프로그램과 완전히 동일해야
# 하므로, 별도 툴체인을 새로 만들지 않고 userland/CMakeLists.txt가
# 이 디렉터리를 out-of-tree add_subdirectory로 끌어들여 같은
# minicore-userland 빌드(같은 툴체인/링커 스크립트)를 그대로 쓴다 -
# 이 파일은 그 빌드 트리 안에서만 구성된다(단독으로 configure 불가).
add_executable(init main.cpp)

set_target_properties(init PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(init PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(init PRIVATE libmc)
