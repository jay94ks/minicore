# minicore/dbgdriver - PN-0556C759(멀티스레드 하드웨어 브레이크포인트
# 경쟁 재현) 전용 디버거 드라이버. minicore/dbgtarget과 같은 이유로
# 일반 유저 앱과 완전히 같은 실행 파일 형태(ET_EXEC, 0x400000, 비-PIE)
# 를 써야 해서 userland/ 빌드 트리를 그대로 재사용한다(out-of-tree
# add_subdirectory 관례 - 이 파일도 그 빌드 트리 안에서만 구성된다,
# 단독 configure 불가). dbgtarget/proctest와 달리 부팅 매니페스트에
# TEMP로 포함될 수 있다(main.cpp 상단 주석 참고) - libcpio로 initrd를
# 직접 읽어야 해서 libmc 외에 libcpio도 링크한다.
add_executable(dbgdriver main.cpp)

set_target_properties(dbgdriver PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(dbgdriver PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(dbgdriver PRIVATE libmc libcpio)
