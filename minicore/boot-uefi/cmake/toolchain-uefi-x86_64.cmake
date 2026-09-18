# UEFI 전용 x86_64 툴체인(PN-7FBF255A) - 커널/유저랜드 툴체인과
# 달리 x86_64-elf 크로스 GCC/Binutils가 아니라 이 WSL 환경의 시스템
# clang(Ubuntu clang, 임의 타깃 트리플 지원)을 그대로 쓴다 -
# x86_64-elf 크로스 바이너리는 PE/COFF 출력을 지원하지 않음을 실측
# 확인했다(PN-7FBF255A "실행 가능성 조사" 절 - `x86_64-elf-objcopy
# --info`에 `pei-x86-64` 타깃이 없음). `-target x86_64-unknown-windows`
# 로 컴파일하면 clang이 MS x64 호출 규약(EFI_STATUS EFIAPI efi_main
# (EFI_HANDLE, EFI_SYSTEM_TABLE*)의 첫 인자 RCX/둘째 RDX)을 자동으로
# 맞춰 준다 - 별도 어셈블리 트램폴린이 필요 없다.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(triple x86_64-unknown-windows)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# [실측 확인] CMAKE_CXX_COMPILER_TARGET만으로는 이 CMAKE_SYSTEM_NAME
# Generic + 플레인 clang 조합에서 컴파일/링크 명령줄 어느 쪽에도
# --target이 실제로 반영되지 않았다(ninja -t commands로 직접 확인) -
# 컴파일 결과가 조용히 ELF로 나와 COFF 링커(lld-link)가 "unknown file
# type"으로 거부했다. CMake 버전/제너레이터에 기대지 않고 두 단계
# 모두에 --target을 직접 주입해 확실히 한다.
set(_minicore_uefi_flags
    "--target=${triple} -ffreestanding -fshort-wchar -mno-red-zone")

set(CMAKE_C_FLAGS_INIT "${_minicore_uefi_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_minicore_uefi_flags} -fno-exceptions -fno-rtti")

set(CMAKE_EXE_LINKER_FLAGS_INIT "--target=${triple} -nostdlib -fuse-ld=lld")
