cmake_minimum_required(VERSION 3.20)
project(minicore CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 서브디렉터리 전부에 공통 적용 - 각 모듈 CMakeLists.txt에서 따로
# 반복하지 않는다. -g(DWARF 디버그 정보)는 PN-1E7798AF(QEMU gdb stub
# 디버깅 워크플로) 요구사항 - 런타임 코드 생성에는 전혀 영향 없고
# ELF에 디버그 섹션만 추가한다(최적화 레벨 미지정 -> 기본 -O0이라
# 소스 대응도 정확하다).
add_compile_options(-Wall -Wextra -g)

set(MINICORE_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/minicore/arch/x86_64/linker.ld)

# PL-2D3184BC 3단계 - Task 커널 스택 언더플로우 감지용 가드 페이지.
# 디버그 빌드 전용(기본 꺼짐) - 켜지면 스택마다 direct map 밖 전용
# 가상주소에 4KiB 단위로 개별 매핑하고 바로 아래 한 페이지를 의도적
# 으로 비워 둔다(task.cpp 참고) - 꺼지면 지금처럼 direct map 안에서
# 컴팩트하게(추가 페이지테이블 없이) 스택을 쓴다.
option(MINICORE_TASK_STACK_GUARD_PAGE "Task 커널 스택 가드 페이지 활성화(디버그 전용)" OFF)

# 모듈별 CMakeLists.txt (SP-8B6B8D25 §3 디렉터리 배치와 1:1 대응) -
# 새 모듈(devmgr/fs/net/tty 등, 전부 유저랜드)이 생기면 여기 목록에
# 추가한다. 전부 OBJECT 라이브러리라 실제 아카이빙/심볼가지치기 없이
# 오브젝트가 그대로 최종 실행 파일에 들어간다.
add_subdirectory(minicore/libs/libkenv)
add_subdirectory(minicore/libs/libcpio)
add_subdirectory(minicore/libs/libkmm)
add_subdirectory(minicore/libs/libelf)
add_subdirectory(minicore/libs/libkcont)
add_subdirectory(minicore/libs/libutf8)
add_subdirectory(minicore/libs/libjson)
add_subdirectory(minicore/libs/x86_64)
add_subdirectory(minicore/arch/x86_64)
add_subdirectory(minicore/kernel)

# [신규, 2026-09-20, SP-43331889 §7, QU-23B339AB/QU-ECEE5990] devmgr -
# 커널에 완전 흡수돼(Process 없는 순수 커널 Task) 더 이상 별도
# 유저랜드 ELF가 아니다 - minicore/devmgr/CMakeLists.txt 문서 주석
# 참고. fs는 아직 유저랜드에 남아 있다(AllocDmaBuffer의 커널 모드
# 매핑 설계가 아직 미정 - SP-43331889 §3-1).
add_subdirectory(minicore/devmgr)

add_executable(minicore.elf)
target_link_libraries(minicore.elf PRIVATE
    minicore_arch_x86_64
    minicore_kernel
    minicore_devmgr
    libkenv
    libcpio
    libkmm
    libelf
    libutf8
    libjson
    minicore_libs_x86_64
)

set_target_properties(minicore.elf PROPERTIES
    LINK_DEPENDS ${MINICORE_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(minicore.elf PRIVATE -T ${MINICORE_LINKER_SCRIPT})
