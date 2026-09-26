# minicore/sockinherit - PN-CC0F4EAC(항목7, fd 상속 실측 검증) 전용
# 최소 테스트 프로그램. minicore/socktest와 같은 이유로 일반 유저 앱과
# 완전히 같은 실행 파일 형태(ET_EXEC, 0x400000, 비-PIE)를 써야 해서
# userland/ 빌드 트리를 그대로 재사용한다(out-of-tree add_subdirectory
# 관례). **의도적으로 부팅 매니페스트(scripts/build-initrd.sh의
# kSpawnServiceProcesses 고정 스폰 목록)에는 포함하지 않는다** -
# main.cpp 상단 주석 참고, kmain.cpp의 TEMP 스폰 경로로만 실행된다.
add_executable(sockinherit main.cpp)

set_target_properties(sockinherit PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(sockinherit PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(sockinherit PRIVATE libmc libcpio)
