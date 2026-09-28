# minicore/diskmetatest - PN-2A0981B7 항목3(ext4/FAT류 Stat/Chmod/Chown
# 디스크 이미지 실측 검증) 전용 클라이언트. vfsmetatest와 동일한
# out-of-tree add_subdirectory 관례 - 부팅 매니페스트에는 포함하지
# 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
add_executable(diskmetatest main.cpp)

set_target_properties(diskmetatest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(diskmetatest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(diskmetatest PRIVATE libmc libcenv)
