# minicore/pubreg - "프로세스간 공개 인터페이스" Registry 서비스 프로세스
# (SP-B071E628, PN-185406F6). minicore/init과 같은 이유(SP-9DD4F3EA
# §4a-3류 "인스턴스는 무조건 1개") - 일반 유저 앱과 완전히 같은 실행 파일
# 형태(ET_EXEC, 0x400000, 비-PIE)를 써야 해서 userland/ 빌드 트리를 그대로
# 재사용한다(minicore/init/CMakeLists.txt와 동일한 out-of-tree 관례 -
# 이 파일도 그 빌드 트리 안에서만 구성된다, 단독 configure 불가. devmgr은
# 더 이상 이 out-of-tree 방식이 아니다 - minicore/kernel/CMakeLists.txt
# 에 직접 소스로 들어간다, PN-D6A05E78).
add_executable(pubreg main.cpp)

set_target_properties(pubreg PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(pubreg PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
# libcenv(신규, PN-185406F6 항목4) - 연결/등록 테이블 관리 코드가
# 컴파일러 생성 memcpy/memset 호출을 필요로 한다(userland/libs/libcenv
# 참고).
target_link_libraries(pubreg PRIVATE libmc libcenv)
