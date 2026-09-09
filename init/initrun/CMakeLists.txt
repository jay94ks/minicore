# 커널이 직접 기동하는 최초 유저 프로세스 (ADR-017,
# docs/plan/kernel-bootstrap.md M8, docs/spec/boot.md §4/§6).
# main.cpp 상단 주석 참고 — 콘솔 출력 없이 커널에 IPC Call 하나를
# 보내는 최소 데모다. x86_64 전용(M1~M8 범위, ADR-009)이라 aarch64
# 빌드에서는 인터페이스 스텁만 유지한다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_initrun INTERFACE)
  return()
endif()

add_executable(minicore_initrun_elf
    main.cpp
    cpio_reader.cpp
    ini_parser.cpp
    panic_stub.cpp
    virtio_blk.cpp
)

target_include_directories(minicore_initrun_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/include
)

target_link_libraries(minicore_initrun_elf PRIVATE minicore_libk)

# 유저 프로세스도 SSE/MMX를 쓰지 않는다 — 이 마일스톤 어디서도
# CR4.OSFXSR을 설정하지 않아(IDT/#NM 처리 없음, kernel/arch/x86_64/
# CMakeLists.txt와 같은 이유) 실제로 쓰면 즉시 폴트한다. 레드존은
# 커널과 달리 걱정할 필요 없다(인터럽트가 이 스레드의 스택 위에서
# 비동기로 실행될 일이 없다 — IDT 자체가 없다) — 그래도 커널과 같은
# 코드 생성 관례를 맞춰 둔다.
target_compile_options(minicore_initrun_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

# kernel/arch/x86_64/CMakeLists.txt와 같은 이유(Windows 호스트에서
# clang++ 링크 드라이버가 --target=*-unknown-none-elf 링크를 POSIX
# 호스트용 g++에 잘못 위임한다) — ld.lld를 직접 호출한다.
set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_initrun_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_initrun_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)

# tools/mkinitrd.py로 이 실행파일을 MCPACK v1 initrd 이미지로 패키징한다
# (boot.md §5). kernel/arch/x86_64가 이 이미지를 .incbin으로 커널 자신에
# 실어 QEMU 데모에 쓴다 — 이 개발 머신에 GRUB가 없어(ADR-114) 진짜
# Multiboot2 모듈 전달 경로를 검증할 수 없는 것의 연장으로, 커널
# 이미지에 직접 심는 이 방식은 개발 환경 한정 대체 수단이다
# (kernel/arch/x86_64/initrd_blob.S.in 주석 참고).
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(MINICORE_INITRD_IMAGE ${CMAKE_CURRENT_BINARY_DIR}/initrd.img)
add_custom_command(
    OUTPUT ${MINICORE_INITRD_IMAGE}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mkinitrd.py
            ${MINICORE_INITRD_IMAGE}
            --disk-cfg=0,4,0,0
            initrun=$<TARGET_FILE:minicore_initrun_elf>
            preempt_busy=$<TARGET_FILE:minicore_preempt_busy_elf>
            preempt_counter=$<TARGET_FILE:minicore_preempt_counter_elf>
    DEPENDS minicore_initrun_elf minicore_preempt_busy_elf minicore_preempt_counter_elf
            ${CMAKE_SOURCE_DIR}/tools/mkinitrd.py
    COMMENT "Packaging initrd.img (MCPACK v1) from initrun.elf + M21 preempt demo binaries"
    VERBATIM
)
add_custom_target(minicore_initrd_image DEPENDS ${MINICORE_INITRD_IMAGE})

# 다른 서브프로젝트(kernel/arch/x86_64)가 이 경로를 configure_file로
# 참조할 수 있도록 전역 프로퍼티로 공개한다 — 경로 문자열이라
# target_compile_definitions보다 GLOBAL PROPERTY가 더 자연스럽다.
set_property(GLOBAL PROPERTY MINICORE_INITRD_IMAGE_PATH ${MINICORE_INITRD_IMAGE})
