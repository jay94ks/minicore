# UEFI 진입 스텁 — PE32+ EFI 애플리케이션(docs/spec/boot.md §1.2,
# docs/design/boot-and-drivers.md ADR-174). minicore_kernel_x86_64(ELF)
# 과는 완전히 별개의 실행 파일이다 — efi_main.cpp 상단 주석 참고.
# x86_64 전용(다른 아키텍처의 UEFI 스텁은 아직 없다).
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_kernel_x86_64_efi INTERFACE)
  return()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# 커널 ELF(minicore_kernel_x86_64, 같은 트리의 상위 CMakeLists.txt가
# 정의)를 이 스텁 안에 통째로 심는다 — kernel_blob.S.in 상단 주석
# 참고. 그 타깃의 실제 출력 경로가 필요하므로 add_subdirectory 순서상
# 이 서브디렉터리가 kernel/arch/x86_64/CMakeLists.txt 뒤에 추가돼야
# 한다(같은 파일의 add_subdirectory(efi_stub) 호출 위치 참고).
# $<TARGET_FILE:...> 제네레이터 표현식은 configure_file()에서 평가되지
# 않는다(제네레이터 표현식은 "생성" 단계 전용, configure_file은 "구성"
# 단계에서 즉시 텍스트를 치환한다) — 그래서 여기서는 실제 출력 경로를
# 문자열로 직접 구성한다(minicore_kernel_x86_64 쪽 SUFFIX=".elf" 설정과
# 반드시 일치해야 한다).
set(MINICORE_KERNEL_ELF ${CMAKE_BINARY_DIR}/kernel/arch/x86_64/minicore_kernel_x86_64.elf)
set(MINICORE_EFI_ENTRY_HEADER ${CMAKE_CURRENT_BINARY_DIR}/efi_entry_addr.hpp)

configure_file(kernel_blob.S.in ${CMAKE_CURRENT_BINARY_DIR}/kernel_blob.S @ONLY)

# _efi_entry(boot.S)의 물리주소를 커널 ELF가 다 만들어진 뒤에
# llvm-nm으로 뽑는다(gen-efi-entry-addr.py 상단 주석) — 이 커널은
# ASLR/PIE가 없어 재현 가능하다.
add_custom_command(
    OUTPUT ${MINICORE_EFI_ENTRY_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen-efi-entry-addr.py
            ${MINICORE_KERNEL_ELF} ${MINICORE_EFI_ENTRY_HEADER}
    DEPENDS minicore_kernel_x86_64 ${CMAKE_SOURCE_DIR}/tools/gen-efi-entry-addr.py
    COMMENT "Extracting _efi_entry physical address from minicore_kernel_x86_64.elf"
    VERBATIM
)
add_custom_target(minicore_efi_entry_header DEPENDS ${MINICORE_EFI_ENTRY_HEADER})

add_executable(minicore_kernel_x86_64_efi
    efi_main.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/kernel_blob.S
)

add_dependencies(minicore_kernel_x86_64_efi minicore_efi_entry_header)

# kernel_blob.S의 .incbin이 실제로 그 시점에 파일이 존재하길 요구한다
# (initrd_blob.S.in과 같은 이유, kernel/arch/x86_64/CMakeLists.txt
# 참고) — 순서는 add_dependencies로 보장한다. initrd_blob.S.in이
# 추가로 쓰는 OBJECT_DEPENDS(내용이 바뀌면 재컴파일 강제)는 여기서는
# 뺐다 — $<TARGET_FILE:...> 제네레이터 표현식이 OBJECT_DEPENDS에서는
# 그대로 평가되지 않아(ninja가 리터럴 문자열을 타깃 이름으로 오인) 이
# 타깃에서는 재현되지 않는다 — 커널을 다시 빌드했는데 efi_stub 쪽
# 파일은 하나도 안 건드린 경우, 수동으로 clean하거나 kernel_blob.S.in
# 자체를 다시 touch해야 갱신된다(알려진 제약).
add_dependencies(minicore_kernel_x86_64_efi minicore_kernel_x86_64)

# 이 타깃 하나만 x86_64-unknown-windows(PE/COFF) 타깃으로 컴파일한다 —
# 커널 본체(ELF)와 링커·오브젝트 포맷이 완전히 다르다. UEFI 호출
# 규약(Microsoft x64 ABI)이 이 타깃의 기본값이라 efi_types.hpp의
# 명시적 ms_abi 속성은 방어적 표기일 뿐이다.
target_compile_options(minicore_kernel_x86_64_efi PRIVATE
    -target x86_64-unknown-windows
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-stack-protector
)

target_include_directories(minicore_kernel_x86_64_efi PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

# lld-link(MSVC 호환 링크 드라이버)로 PE32+ EFI 애플리케이션을 만든다
# — ld.lld(ELF 전용)로는 이 출력 포맷 자체를 못 만든다. /ENTRY는
# efi_main(ms_abi) — UEFI가 이 심볼을 (EFI_HANDLE, EFI_SYSTEM_TABLE*)
# 로 직접 호출한다.
set(CMAKE_CXX_LINK_EXECUTABLE
    "lld-link /machine:x64 /subsystem:efi_application /entry:efi_main /nodefaultlib /out:<TARGET> <OBJECTS>"
)

set_target_properties(minicore_kernel_x86_64_efi PROPERTIES
    LINKER_LANGUAGE CXX
    SUFFIX ".efi"
)
