# x86_64 arch 계층 (ADR-002/009/017) — Multiboot2 부트 스텁, 실제
# 부팅 가능한 커널 ELF는 이 서브프로젝트가 링크한다.
#
# docs/plan/kernel-bootstrap.md M1: QEMU 시리얼 콘솔에 "hello from
# kernel" 출력. docs/spec/virtual-memory-layout.md §2의 physmap/커널
# 이미지 higher-half 매핑을 boot/paging_setup.cpp가 구성한다.
# M2: boot_info_x86_64.cpp가 Multiboot2 태그를 파싱한다(boot.md §2~3).
# M3: irq.cpp가 libk::irq_safe<Lock>의 arch_irq_save/restore 훅(ADR-076)을
# 제공한다.
# M4: page_table.cpp가 임의 주소공간(PML4)에 대한 매핑/해제/권한 변경
# API를 제공한다.
# M5: context_switch.S가 kernel/core/sched의 arch_context_switch 훅을
# 제공한다.
# M8: elf_loader.cpp(ELF64 PT_LOAD 로더), syscall_entry.S+syscall.cpp
# (SYSCALL/SYSRET), usermode.S+user_thread.cpp(IRETQ로 최초 유저모드
# 진입) — initrd_blob.S(아래 configure_file)는 개발용 initrd 임베딩.

# init/initrun/CMakeLists.txt가 만든 initrd.img 경로를 읽어 .incbin
# 소스를 생성한다 — 최상위 CMakeLists.txt가 init/initrun을 kernel보다
# 먼저 add_subdirectory해 둔 덕분에 이 시점에 프로퍼티가 이미 설정돼
# 있다(initrd_blob.S.in 주석 참고).
get_property(MINICORE_INITRD_IMAGE GLOBAL PROPERTY MINICORE_INITRD_IMAGE_PATH)
configure_file(initrd_blob.S.in ${CMAKE_CURRENT_BINARY_DIR}/initrd_blob.S @ONLY)

add_executable(minicore_kernel_x86_64
    boot/boot.S
    boot/paging_setup.cpp
    boot_info_x86_64.cpp
    context_switch.S
    elf_loader.cpp
    idle.S
    irq.cpp
    kernel_main.cpp
    klog_uart.cpp
    page_table.cpp
    syscall.cpp
    syscall_entry.S
    usermode.S
    user_thread.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/initrd_blob.S
)

# initrd_blob.S의 .incbin이 실제로 그 시점에 파일이 존재하길 요구한다 —
# configure_file은 경로 문자열만 넣을 뿐 파일 내용(빌드 결과물)까지
# 기다려 주지 않으므로 빌드 순서를 명시적으로 강제한다.
add_dependencies(minicore_kernel_x86_64 minicore_initrd_image)

# add_dependencies만으로는 "순서"만 보장된다 — ninja는 .incbin 안쪽까지
# 들여다보지 않으므로, initrd.img "내용"이 바뀌어도 이미 컴파일된
# initrd_blob.S.obj를 그대로 재사용해 버린다(실제로 겪은 문제 —
# mkinitrd.py를 고쳐 initrd.img가 다시 만들어졌는데도 커널 바이너리에
# 반영되지 않았다). OBJECT_DEPENDS로 그 obj가 이 파일에도 의존한다고
# 명시해야 재컴파일이 트리거된다.
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/initrd_blob.S PROPERTIES
    OBJECT_DEPENDS ${MINICORE_INITRD_IMAGE}
)

# ADR-010 공통 정책(-ffreestanding 등)은 toolchain/common.cmake가 이미
# 전역 적용한다. 여기서는 x86_64에만 필요한 추가 제약만 얹는다:
#   -mno-red-zone   인터럽트가 언제든 스택 위 128바이트를 덮어쓸 수 있음
#                   (레드존 미보장, freestanding 커널의 표준 요구사항).
#   -mno-*(SSE류)   FPU/SSE 상태를 아직 초기화하지 않았으므로(CR4.OSFXSR
#                   미설정) 컴파일러가 XMM 레지스터를 쓰지 못하게 막는다.
target_compile_options(minicore_kernel_x86_64 PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

target_link_libraries(minicore_kernel_x86_64 PRIVATE minicore_kernel_core)

# kernel/core/mm의 phys_map.hpp가 <arch_mm_defs.hpp>(제네릭 이름)로
# 참조하는 이 디렉토리 자신의 헤더를 찾을 수 있어야 한다 — 앵글 브래킷
# include는 "포함하는 파일과 같은 디렉토리"를 자동으로 보지 않으므로
# 명시적으로 이 디렉토리를 include 경로에 얹는다.
target_include_directories(minicore_kernel_x86_64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# clang++를 링크 드라이버로 쓰면(host가 Windows이므로) --target=*-unknown-none-elf를
# 스스로 링크할 방법을 못 찾고 PATH의 g++(MSYS2, POSIX 호스트용)에 위임해버려
# "-T"조차 못 알아듣는 상태가 된다 — freestanding ELF에는 애초에 불필요한
# 우회이므로, ld.lld를 CMake가 직접 호출하도록 링크 규칙 자체를 바꾼다.
set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_kernel_x86_64 PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_kernel_x86_64 PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
