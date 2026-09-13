set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(triple x86_64-unknown-none-elf)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_ASM_COMPILER_TARGET ${triple})

# 부트로더가 넘겨준 링크 상태(크로스컴파일 + 프리스탠딩)에서는 CMake의
# 기본 컴파일러 점검(실행 파일 링크 시도)이 실패한다 - 정적 라이브러리
# 링크로 낮춰서 툴체인 자체는 정상 동작함을 확인하게 한다.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_minicore_freestanding_flags
    "-ffreestanding -fno-stack-protector -fno-pic -fno-pie \
-mno-red-zone -mcmodel=kernel -mgeneral-regs-only")

set(CMAKE_C_FLAGS_INIT "${_minicore_freestanding_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_minicore_freestanding_flags} -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS_INIT "-ffreestanding")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -static -fuse-ld=lld")
