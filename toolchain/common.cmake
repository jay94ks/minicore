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
#
# -isystem freestanding-cxx: 이 툴체인에는 freestanding 타깃용 libc++가
# 없어 <cstdint>/<cstddef>/<cstdarg>가 어디에도 없다 — 자체 shim으로
# 보강한다(ADR-113, OPEN-48). 컴파일러 내장 헤더보다 먼저 탐색되어야
# 하므로 -isystem으로 추가한다.
set(MINICORE_COMMON_COMPILE_OPTIONS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-stack-protector
    -fno-pic
    -isystem ${CMAKE_CURRENT_LIST_DIR}/freestanding-cxx
    -Wall
    -Wextra
)

add_compile_options(${MINICORE_COMMON_COMPILE_OPTIONS})
add_link_options(-nostdlib -static)

# cxx-conventions.md §1이 <concepts>를 허용 목록에 넣은 것 자체가
# C++20 이상을 전제한다 — 명시적으로 고정해 clang 버전에 따라 기본
# 표준이 달라지는 데 의존하지 않는다.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
