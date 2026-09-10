# minicore SDK 최소 CMake 툴체인 파일 (real-libc-syscall-layer.md
# §M38, docs/design/build-system.md ADR-190). 저장소 밖 프로젝트가
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=<이 SDK 경로>/x86_64-minicore.cmake ..
#
# 하나만 지정하면 되도록 컴파일 플래그(타깃 트리플+include 경로)와
# 정적 링크(ld.lld 직접 호출, link.ld)를 미리 박아 둔다 — 저장소
# 안의 toolchain/x86_64-clang.cmake(freestanding 커널/서버 빌드용)
# 와는 별개다(같은 clang 설치를 재사용하지만 용도가 다르다).
#
# bin/x86_64-minicore-clang(bash 스크립트)를 CMAKE_C_COMPILER로
# 직접 가리키지 않는다 — cmake/ninja는 컴파일러를 OS 프로세스
# 실행기로 직접 실행하는데(셸을 거치지 않는다), Windows에서는 bash
# 스크립트를 그렇게 실행할 수 없다("%1 is not a valid Win32
# application", M38 실행 중 실제로 겪음). 그 스크립트는 명령줄에서
# 사람이 직접(또는 Makefile 등에서 셸을 거쳐) 쓰는 용도로 남겨 두고,
# CMake 경로는 이 파일이 플래그를 직접 주입해 clang을 그대로
# 부른다(toolchain/x86_64-clang.cmake와 같은 방식 — 이게 실제로
# 크로스플랫폼에서 검증된 경로다).
get_filename_component(MINICORE_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER clang)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET x86_64-unknown-none-elf)
set(CMAKE_ASM_COMPILER_TARGET x86_64-unknown-none-elf)

# CMake의 컴파일러 정상성 검사(try_compile)는 실행 파일을 만들어
# 보려 하는데, 이 타깃은 링크에 커스텀 link.ld+ld.lld 직접 호출이
# 필요해 그 기본 시도가 항상 실패한다 — 저장소 안
# toolchain/common.cmake와 같은 이유로 끈다.
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)

set(CMAKE_C_FLAGS_INIT
    "-ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-3dnow \
-I${MINICORE_SDK_ROOT}/include/musl/arch/x86_64 \
-I${MINICORE_SDK_ROOT}/include/musl/arch/generic \
-I${MINICORE_SDK_ROOT}/include/musl/generated \
-I${MINICORE_SDK_ROOT}/include/musl/include \
-I${MINICORE_SDK_ROOT}/include"
)

# userland/musl-hello/CMakeLists.txt와 완전히 같은 패턴 — clang을
# 링크 드라이버로 쓰지 않고 ld.lld를 직접 부른다(clang의 링크
# 모드는 자기 자신의 기본 crt/libc 탐색을 가정해서, 이 SDK의 정적
# libc.a+커스텀 link.ld 조합과 맞지 않는다).
set(CMAKE_C_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 -static --build-id=none -T ${MINICORE_SDK_ROOT}/link.ld -o <TARGET> <OBJECTS> <LINK_LIBRARIES> -L${MINICORE_SDK_ROOT}/lib -lc -lmc"
)
