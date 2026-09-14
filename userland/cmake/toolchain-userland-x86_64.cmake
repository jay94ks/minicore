# 유저랜드 전용 x86_64 툴체인(SP-8B6B8D25 §5-A 확정, QU-FF3F0CAA 답변
# - "userland/{apps,libs,tests}" 체계) - 커널 툴체인
# (minicore/cmake/toolchain-x86_64.cmake에 대응하는 루트 cmake/
# toolchain-x86_64.cmake)과 같은 프리스탠딩 x86_64-unknown-none-elf
# 타깃을 쓰지만, 컴파일 플래그는 다르다:
#
# - `-mcmodel=kernel`(심볼이 상위 2GiB에 있다는 전제) 없음 - 유저
#   프로그램은 저지대 고정 베이스 0x400000(§5-A)에 링크되므로 기본
#   코드 모델(small, 2GiB 이내 가정)이 그대로 맞는다.
# - `-mno-red-zone` 없음 - 이 제약은 "인터럽트 핸들러가 언제든 현재
#   스택 위에 끼어들 수 있는" 커널 컨텍스트에만 필요하다(레드존을 밟는
#   인터럽트 핸들러가 스택을 훼손하는 문제). 유저 프로그램은 syscall/
#   인터럽트 진입 시 커널이 이미 다른 스택(TSS.RSP0/커널 스택)으로
#   전환하므로 이 문제가 없다.
#
# 커널과 유저랜드는 이렇게 서로 다른 툴체인 설정이 필요해 애초에
# 별도 CMake 빌드 트리(userland/ 자신이 루트 CMakeLists.txt를 가진
# 독립 프로젝트)로 뒀다 - 하나의 `cmake --build`로 양쪽을 동시에
# 만들 수 없다(RM-7C249618 "Minicore 라이브러리 목록" 참고).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(triple x86_64-unknown-none-elf)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_ASM_COMPILER_TARGET ${triple})

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_minicore_userland_flags
    "-ffreestanding -fno-stack-protector -fno-pic -fno-pie -mgeneral-regs-only")

set(CMAKE_C_FLAGS_INIT "${_minicore_userland_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_minicore_userland_flags} -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS_INIT "-ffreestanding")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -static -fuse-ld=lld")
