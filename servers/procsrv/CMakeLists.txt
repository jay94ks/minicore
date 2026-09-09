# 프로세스 서버: fork/exec/signal/wait/pid, fd 진실 공급원 (ADR-016).
# M12(system-servers-bringup.md) 골격 — main.cpp 상단 주석 참고.
# init/initrun/CMakeLists.txt와 같은 이유로 x86_64 전용이다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_procsrv INTERFACE)
  return()
endif()

add_executable(minicore_procsrv_elf
    main.cpp
)

target_include_directories(minicore_procsrv_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
)

target_link_libraries(minicore_procsrv_elf PRIVATE minicore_libk)

# init/initrun/CMakeLists.txt와 같은 이유(SSE/MMX 미사용, red zone 불필요).
target_compile_options(minicore_procsrv_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

# init/initrun/CMakeLists.txt와 같은 이유(Windows 호스트 clang++ 링크
# 드라이버 우회) — ld.lld를 직접 호출한다.
set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_procsrv_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_procsrv_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)

# tools/mkbootdisk.py가 이 실행파일을 부트 디스크 cpio 이미지에
# bin/procsrv로 담는다(repo-layout.md, system-servers-bringup.md §M12) —
# init/initrun의 initrd.img(커널이 .incbin으로 자신에 심는 것)와는
# 완전히 별개의, virtio-blk로 붙는 진짜 디스크 이미지다.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(MINICORE_BOOTDISK_IMAGE ${CMAKE_CURRENT_BINARY_DIR}/bootdisk.img)
add_custom_command(
    OUTPUT ${MINICORE_BOOTDISK_IMAGE}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mkbootdisk.py
            ${MINICORE_BOOTDISK_IMAGE}
            --service=procsrv=$<TARGET_FILE:minicore_procsrv_elf>
    DEPENDS minicore_procsrv_elf ${CMAKE_SOURCE_DIR}/tools/mkbootdisk.py
    COMMENT "Packaging bootdisk.img (cpio newc) from procsrv.elf"
    VERBATIM
)
add_custom_target(minicore_bootdisk_image DEPENDS ${MINICORE_BOOTDISK_IMAGE})
set_property(GLOBAL PROPERTY MINICORE_BOOTDISK_IMAGE_PATH ${MINICORE_BOOTDISK_IMAGE})
