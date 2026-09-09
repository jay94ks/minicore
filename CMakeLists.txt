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
#
# init/initrun을 kernel보다 먼저 추가한다: M8(kernel-bootstrap.md)부터
# 커널이 initrun.elf를 MCPACK으로 패키징한 이미지를 .incbin으로 자신에
# 심는다(kernel/arch/x86_64/initrd_blob.S.in) — 그 파일 경로를 담는
# GLOBAL PROPERTY(init/initrun/CMakeLists.txt가 설정)를 kernel/arch/x86_64가
# configure_file 시점에 읽어야 하므로, 이 순서가 아니면 아직 설정되지
# 않은 프로퍼티를 읽게 된다.
add_subdirectory(libk)
add_subdirectory(init/initrun)
add_subdirectory(kernel)

# 1차 타겟(x86_64)만 전체 스택(시스템 서버·libc·유저랜드)을 빌드 대상으로 삼는다.
# aarch64는 현재 범위에서 커널 컴파일 검증까지만 다룬다 (ADR-009).
if(MINICORE_ARCH STREQUAL "x86_64")
  add_subdirectory(libmc)
  # userland를 servers보다 먼저 추가한다 — M20(system-servers-bringup.md
  # §M20)부터 부트 디스크 조립(servers/CMakeLists.txt)이 userland/shell의
  # $<TARGET_FILE:minicore_shell_elf>를 참조한다(그 반대 참조는 없다).
  add_subdirectory(userland)
  add_subdirectory(servers)
  add_subdirectory(libc)
endif()
