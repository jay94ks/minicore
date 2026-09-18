# minicore/dbgtarget - PN-87D6B615 "남은 범위" 2번(실제 initrd 기반
# 디버그 세션 E2E) 전용 최소 디버기 프로그램. minicore/pubreg 등과
# 같은 이유로 일반 유저 앱과 완전히 같은 실행 파일 형태(ET_EXEC,
# 0x400000, 비-PIE)를 써야 해서 userland/ 빌드 트리를 그대로
# 재사용한다(out-of-tree add_subdirectory 관례 - 이 파일도 그 빌드
# 트리 안에서만 구성된다, 단독 configure 불가). **의도적으로 부팅
# 매니페스트(scripts/build-initrd.sh, kmain.cpp gServiceManifest)에는
# 포함하지 않는다** - main.cpp 상단 주석 참고.
add_executable(dbgtarget main.cpp)

set_target_properties(dbgtarget PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(dbgtarget PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(dbgtarget PRIVATE libmc)
