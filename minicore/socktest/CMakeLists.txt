# minicore/socktest - PN-CC0F4EAC(소켓 계층 실측 검증) 전용 최소
# 테스트 프로그램. minicore/proctest와 같은 이유로 일반 유저 앱과
# 완전히 같은 실행 파일 형태(ET_EXEC, 0x400000, 비-PIE)를 써야 해서
# userland/ 빌드 트리를 그대로 재사용한다(out-of-tree add_subdirectory
# 관례 - 이 파일도 그 빌드 트리 안에서만 구성된다, 단독 configure
# 불가). **의도적으로 부팅 매니페스트(scripts/build-initrd.sh의
# kSpawnServiceProcesses 고정 스폰 목록)에는 포함하지 않는다** -
# main.cpp 상단 주석 참고, kmain.cpp의 TEMP 스폰 경로로만 실행된다.
add_executable(socktest main.cpp)

set_target_properties(socktest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(socktest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
# libcenv - `static mc::uint8_t buf[64]` 같은 값초기화가 컴파일러의
# 암묵적 memset 호출을 만든다(proctest와 동일한 이유, PN-A40787C8 선례).
target_link_libraries(socktest PRIVATE libmc libcenv)
