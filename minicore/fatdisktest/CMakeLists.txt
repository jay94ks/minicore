# minicore/fatdisktest - PN-2A0981B7 항목3(FAT32 실제 디스크 이미지
# Stat/Chmod 실측 검증) 전용 클라이언트. diskmetatest(ext4용)와 동일한
# out-of-tree add_subdirectory 관례 - 부팅 매니페스트에는 포함하지
# 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
add_executable(fatdisktest main.cpp)

set_target_properties(fatdisktest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(fatdisktest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(fatdisktest PRIVATE libmc libcenv)
