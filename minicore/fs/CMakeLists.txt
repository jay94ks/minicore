# minicore/fs - VFS 마운트 지점 실질 처리 서비스 프로세스(SP-8B6B8D25
# §4-A, PN-452FF696). minicore/devmgr/pubreg/init과 같은 이유(SP-9DD4F3EA
# §4a-3류 "인스턴스는 무조건 1개") - 일반 유저 앱과 완전히 같은 실행 파일
# 형태(ET_EXEC, 0x400000, 비-PIE)를 써야 해서 userland/ 빌드 트리를 그대로
# 재사용한다(minicore/pubreg/CMakeLists.txt와 동일한 out-of-tree 관례 -
# 이 파일도 그 빌드 트리 안에서만 구성된다, 단독 configure 불가).
add_executable(fs main.cpp ahci.cpp)

set_target_properties(fs PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(fs PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
# libcenv(PN-4E6EA13D/QU-1FB6A7A4, ahci.cpp - devmgr에서 이관) - 커맨드
# 헤더/PRDT/FIS 구조체를 매번 제로 초기화하는 memset 호출이 컴파일러
# 생성으로 필요해진다(pubreg의 선례와 동일한 이유, userland/libs/
# libcenv 참고).
target_link_libraries(fs PRIVATE libmc libcenv)
