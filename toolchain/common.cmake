# 공통 툴체인 설정 (ADR-010: 예외/RTTI 없음, freestanding 빌드)
#
# 아키텍처×컴파일러별 toolchain 파일(x86_64-clang.cmake 등)이
# CMAKE_*_COMPILER 를 정한 뒤 이 파일을 include하여 공통 설정을 적용한다.
# 이 파일 단독으로는 사용하지 않는다.

if(NOT DEFINED MINICORE_TOOLCHAIN_ARCH)
  message(FATAL_ERROR "common.cmake는 arch별 toolchain 파일에서 MINICORE_TOOLCHAIN_ARCH를 설정한 뒤 include해야 한다")
endif()

set(CMAKE_SYSTEM_NAME Generic)                 # 호스트 OS 없는 프리스탠딩 타깃 (ADR-003)
set(CMAKE_SYSTEM_PROCESSOR ${MINICORE_TOOLCHAIN_ARCH})

# 크로스 링커가 아직 준비되지 않은 단계에서도 컴파일러 자체 동작 확인이
# 가능하도록, 컴파일러 감지 시도(try_compile)를 "링크까지"가 아니라
# "컴파일까지"로 낮춘다. 실제 커널 링크는 각 서브프로젝트가 링커 스크립트와
# 함께 명시적으로 수행한다.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ADR-010: 예외/RTTI 금지, freestanding 헤더만 허용
set(MINICORE_COMMON_COMPILE_OPTIONS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-stack-protector
    -fno-pic
    -Wall
    -Wextra
)

add_compile_options(${MINICORE_COMMON_COMPILE_OPTIONS})
add_link_options(-nostdlib -static)
