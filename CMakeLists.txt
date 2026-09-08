cmake_minimum_required(VERSION 3.20)
project(minicore CXX C ASM)

# ADR-021: monorepo + 서브프로젝트(meta-build).
# 이 파일은 "어떤 arch에서 무엇을 빌드하는가"만 결정하고,
# 실제 컴파일 규칙은 각 서브프로젝트의 CMakeLists.txt가 소유한다.

set(MINICORE_ARCH "x86_64" CACHE STRING "Target architecture (x86_64|aarch64)")
set_property(CACHE MINICORE_ARCH PROPERTY STRINGS x86_64 aarch64)

if(NOT MINICORE_ARCH STREQUAL "x86_64" AND NOT MINICORE_ARCH STREQUAL "aarch64")
  message(FATAL_ERROR "지원하지 않는 MINICORE_ARCH: ${MINICORE_ARCH} (x86_64 | aarch64만 지원, ADR-009)")
endif()

# 커널과 공용 코어 라이브러리는 모든 아키텍처에서 빌드한다 (ADR-002, ADR-009).
add_subdirectory(libk)
add_subdirectory(kernel)
add_subdirectory(init/initrun)

# 1차 타겟(x86_64)만 전체 스택(시스템 서버·libc·유저랜드)을 빌드 대상으로 삼는다.
# aarch64는 현재 범위에서 커널 컴파일 검증까지만 다룬다 (ADR-009).
if(MINICORE_ARCH STREQUAL "x86_64")
  add_subdirectory(servers)
  add_subdirectory(libc)
  add_subdirectory(userland)
endif()
