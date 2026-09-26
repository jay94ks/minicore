# minicore/sockinheritchild - minicore/sockinherit(PN-CC0F4EAC 항목7,
# fd 상속 실측 검증)의 자식 반쪽. minicore/socktest와 같은 이유로
# 일반 유저 앱과 완전히 같은 실행 파일 형태(ET_EXEC, 0x400000, 비-PIE)
# 를 써야 해서 userland/ 빌드 트리를 그대로 재사용한다(out-of-tree
# add_subdirectory 관례). **의도적으로 부팅 매니페스트(scripts/
# build-initrd.sh의 kSpawnServiceProcesses 고정 스폰 목록)에는 포함하지
# 않는다** - sockinherit이 SpawnProcess로 직접 스폰하므로 initrd 안에
# 원본 ELF 바이트로만 실려야 한다(sockclient와 동일한 관례).
add_executable(sockinheritchild main.cpp)

set_target_properties(sockinheritchild PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(sockinheritchild PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
# libcenv - `static mc::uint8_t buf[64]` 같은 값초기화가 컴파일러의
# 암묵적 memset 호출을 만든다(socktest와 동일한 이유, PN-A40787C8 선례).
target_link_libraries(sockinheritchild PRIVATE libmc libcenv)
