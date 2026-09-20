# minicore/authmgr - 사용자 신원 관리(authmgr) 서비스 프로세스
# (SP-8B6B8D25 §3.1 항목8, PN-24A2B6F5/PN-CFEAEF40). minicore/pubreg와
# 같은 이유(SP-9DD4F3EA §4a-3류 "인스턴스는 무조건 1개") - 일반 유저
# 앱과 완전히 같은 실행 파일 형태(ET_EXEC, 0x400000, 비-PIE)를 써야
# 해서 userland/ 빌드 트리를 그대로 재사용한다(minicore/pubreg/
# CMakeLists.txt와 동일한 out-of-tree 관례 - 이 파일도 그 빌드 트리
# 안에서만 구성된다, 단독 configure 불가).
add_executable(authmgr main.cpp)

set_target_properties(authmgr PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(authmgr PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(authmgr PRIVATE libmc libcenv)
