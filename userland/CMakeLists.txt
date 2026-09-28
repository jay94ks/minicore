# Minicore 유저랜드 빌드 루트(SP-8B6B8D25 §5-A, QU-FF3F0CAA 답변 확정)
# - 커널과 완전히 별도의 CMake 빌드 트리다(userland/cmake/
# toolchain-userland-x86_64.cmake로 구성):
#
#   cmake -S userland -B build-userland -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=userland/cmake/toolchain-userland-x86_64.cmake
#   cmake --build build-userland
#
# 디렉터리 체계(설계자 지시, 2026-09-14): apps/ libs/ tests/ 하위에
# 프로젝트 이름별 폴더 - 새 하위 프로젝트를 만들 때마다 add_subdirectory
# 를 추가하고 RM-7C249618("Minicore 라이브러리 목록")도 갱신한다.
cmake_minimum_required(VERSION 3.20)
project(minicore-userland CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

set(MINICORE_USERLAND_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/linker-userland-x86_64.ld)

add_subdirectory(libs/libmc)
add_subdirectory(libs/libcenv)
add_subdirectory(libs/libcpio)

# arch/x86_64(SP-8B6B8D25 §3.0, RM-23F4B687 §3 갱신, PN-E612E714) -
# io_port.h/msr.h는 애초에 유저/커널 공용을 전제로 설계됐다
# (QU-4606360C) - init/pubreg 등과 동일한 이유의 out-of-tree
# add_subdirectory(커널 빌드 트리와 완전히 별개 CMake 프로젝트라
# 양쪽에 각각 add_subdirectory 필요). 이 타깃(OBJECT 라이브러리,
# isr.S/context_switch.S/syscall_entry.S는 커널 전용) 자체를 지금
# 당장 어느 유저랜드 실행 파일이 링크하는 건 아니다 - 헤더 include
# 경로를 노출해 두는 게 목적이고, 실제 컴파일 확인은 아래
# userlandtest에서 별도로 한다(이 OBJECT 타깃을 링크하면 커널 전용
# .S 파일들의 심볼까지 끌려와 불필요한 링크 문제가 생길 수 있어
# 피한다).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/arch/x86_64 ${CMAKE_CURRENT_BINARY_DIR}/minicore-arch-x86_64)

add_subdirectory(tests/userlandtest)

# minicore/init(SP-68182FBD §2.4) - userland/ 밖에 있지만 다른 유저
# 프로그램과 완전히 같은 실행 파일 형태(ET_EXEC, 0x400000, 비-PIE)로
# 빌드해야 해서 이 트리를 그대로 재사용(out-of-tree add_subdirectory,
# 바이너리 디렉터리를 명시해 소스 트리 밖에서도 안전).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/init ${CMAKE_CURRENT_BINARY_DIR}/minicore-init)

# devmgr(SP-9DD4F3EA §6, PN-BD9AAE2F) - **[제외, 2026-09-20,
# SP-43331889/QU-23B339AB/QU-ECEE5990] 더 이상 유저랜드 ELF가 아니다**,
# Process 없는 순수 커널 KernelThread로 완전 흡수돼 이 유저랜드 빌드
# 트리에서 뺐다. **[정리, 2026-09-21, 설계자 지시, PN-D6A05E78]**
# minicore/devmgr 디렉터리 자체가 사라졌다 - 소스는
# minicore/kernel/devmgr.cpp로 옮겨 top-level CMakeLists.txt가 커널
# 빌드 트리(minicore_kernel 소스 목록)에 직접 넣는다.

# pubreg(SP-B071E628, PN-185406F6) - init/devmgr과 동일한 이유의
# out-of-tree add_subdirectory.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/pubreg ${CMAKE_CURRENT_BINARY_DIR}/minicore-pubreg)

# libkvdb(SP-30FCC8AE §1-D, PN-24A2B6F5 §1-C/D 항목1) - authmgr(유저
# 랜드 프로세스)의 의존성이라 이 유저랜드 빌드 트리에도 필요하다.
# 커널 빌드 트리(top-level CMakeLists.txt)와는 완전히 별개 CMake
# 프로젝트라 add_subdirectory를 양쪽에 각각 둬야 한다(순수 헤더
# INTERFACE 라이브러리라 이중 컴파일 걱정은 없음) - init/pubreg와
# 동일한 out-of-tree 관례.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/libs/libkvdb ${CMAKE_CURRENT_BINARY_DIR}/minicore-libkvdb)

# libkcrypto(SP-CC1CF30E §8) - authmgr(유저랜드 프로세스)의 미래
# 의존성(패스워드 SHA256 해싱)이라 libkvdb와 동일한 이유의 out-of-tree
# add_subdirectory - 커널 빌드 트리(top-level CMakeLists.txt)와는
# 완전히 별개 CMake 프로젝트라 양쪽에 각각 둬야 한다.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/libs/libkcrypto ${CMAKE_CURRENT_BINARY_DIR}/minicore-libkcrypto)

# authmgr(SP-8B6B8D25 §3.1 항목8, PN-24A2B6F5/PN-CFEAEF40) -
# init/pubreg와 동일한 이유의 out-of-tree add_subdirectory.
# [갱신, 2026-09-23, PN-24A2B6F5] libkvdb 연동 시작 - 스캐폴딩만
# 이던 v1에서 확장.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/authmgr ${CMAKE_CURRENT_BINARY_DIR}/minicore-authmgr)

# fs(SP-8B6B8D25 §4-A, PN-452FF696) - **[제외, 2026-09-20,
# SP-43331889/QU-5FC58B06] 더 이상 유저랜드 ELF가 아니다**, devmgr과
# 동일한 이유로 Process 없는 순수 커널 KernelThread로 완전 흡수돼
# 이 유저랜드 빌드 트리에서 뺐다. **[정리, 2026-09-21, 설계자 지시,
# PN-D6A05E78]** minicore/fs 디렉터리 자체가 사라졌다 - 소스(main.cpp,
# ahci.h/.cpp, block_device.h)는 전부 minicore/kernel/로 옮겨
# top-level CMakeLists.txt가 커널 빌드 트리에 직접 넣는다.

# dbgtarget(PN-87D6B615 "남은 범위" 2번, 디버그 세션 E2E 전용 디버기) -
# init/devmgr/pubreg/fs와 동일한 이유의 out-of-tree add_subdirectory.
# **부팅 매니페스트(scripts/build-initrd.sh)에는 의도적으로 포함하지
# 않는다** - minicore/dbgtarget/main.cpp 상단 주석 참고.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/dbgtarget ${CMAKE_CURRENT_BINARY_DIR}/minicore-dbgtarget)

# proctest(PN-012D6310, procfs pid 열람 실측 검증 전용) - dbgtarget과
# 동일한 이유의 out-of-tree add_subdirectory. **부팅 매니페스트
# (scripts/build-initrd.sh의 고정 스폰 목록)에는 의도적으로 포함하지
# 않는다** - minicore/proctest/main.cpp 상단 주석 참고.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/proctest ${CMAKE_CURRENT_BINARY_DIR}/minicore-proctest)

# dbgdriver(PN-0556C759, 멀티스레드 하드웨어 브레이크포인트 경쟁
# 재현용 디버거 드라이버) - dbgtarget/proctest와 동일한 이유의
# out-of-tree add_subdirectory. dbgtarget/proctest와 달리 kmain.cpp의
# TEMP 스폰 경로(kSpawnDbgDriverForInvestigation())로 부팅 시 자동
# 실행되므로 이 재현 조사 기간에는 initrd 안에 실려야 한다 -
# minicore/dbgdriver/main.cpp 상단 주석 참고.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/dbgdriver ${CMAKE_CURRENT_BINARY_DIR}/minicore-dbgdriver)

# socktest(PN-CC0F4EAC, 소켓 계층 실측 검증) - dbgtarget/proctest와
# 동일한 이유의 out-of-tree add_subdirectory. dbgdriver와 동일하게
# kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행되므로 그 검증 기간
# 동안은 initrd 안에 실려야 한다 - minicore/socktest/main.cpp 상단
# 주석 참고.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/socktest ${CMAKE_CURRENT_BINARY_DIR}/minicore-socktest)

# sockclient(PN-CC0F4EAC, socktest의 클라이언트 반쪽) - dbgtarget/proctest와
# 동일한 이유의 out-of-tree add_subdirectory. socktest가 SpawnProcess로
# 직접 스폰하므로(dbgdriver가 dbgtarget을 스폰하는 것과 동일한 패턴)
# initrd 안에 원본 ELF 바이트로 실려야 한다 - minicore/sockclient/main.cpp
# 상단 주석 참고.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/sockclient ${CMAKE_CURRENT_BINARY_DIR}/minicore-sockclient)

# sockinherit/sockinheritchild(PN-CC0F4EAC 항목7, fd 상속 실측 검증) -
# dbgdriver/dbgtarget과 동일한 이유의 out-of-tree add_subdirectory 쌍 -
# sockinherit이 진짜 SpawnProcess로 sockinheritchild를 직접 스폰하므로
# initrd 안에 원본 ELF 바이트로 실려야 한다 - 각 main.cpp 상단 주석 참고.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/sockinherit ${CMAKE_CURRENT_BINARY_DIR}/minicore-sockinherit)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/sockinheritchild ${CMAKE_CURRENT_BINARY_DIR}/minicore-sockinheritchild)

# authtest(PN-24A2B6F5, authmgr libkvdb CreateUser/LookupByUid E2E
# 재검증) - dbgdriver/socktest와 동일한 이유의 out-of-tree
# add_subdirectory - kmain.cpp의 TEMP 스폰 경로로 이름("authtest")이
# 매치돼 부팅 시 자동 실행된다(minicore/authtest/main.cpp 상단 주석
# 참고).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/authtest ${CMAKE_CURRENT_BINARY_DIR}/minicore-authtest)

# epolltest(SP-6350DEBB/PN-7562DA62, epoll류 이벤트 다중화 실측 검증) -
# dbgtarget/proctest와 동일한 이유의 out-of-tree add_subdirectory.
# 기존 sockclient(클라이언트 반쪽, "socktest.sock" 이름 하드코딩)를
# 그대로 재사용해 짝을 이룬다 - minicore/epolltest/main.cpp 상단
# 주석 참고. **socktest와 같은 부팅 훅에 절대 함께 켜지 않는다.**
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/epolltest ${CMAKE_CURRENT_BINARY_DIR}/minicore-epolltest)

# setuidtest(DC-90A66932 (A), authmgr_client.h 커널 전용 Channel
# 클라이언트의 캐시미스 왕복 E2E 검증) - authtest와 동일한 이유의
# out-of-tree add_subdirectory - kmain.cpp의 TEMP 스폰 경로로
# 이름("setuidtest")이 매치돼 부팅 시 자동 실행된다(minicore/setuidtest/
# main.cpp 상단 주석 참고).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/setuidtest ${CMAKE_CURRENT_BINARY_DIR}/minicore-setuidtest)

# sudotest(DC-34764C25 항목1, sudo/su authmgr 화이트리스트 E2E 검증) -
# setuidtest와 동일한 이유의 out-of-tree add_subdirectory - kmain.cpp의
# TEMP 스폰 경로로 이름("sudotest")이 매치돼 부팅 시 자동 실행된다
# (minicore/sudotest/main.cpp 상단 주석 참고).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/sudotest ${CMAKE_CURRENT_BINARY_DIR}/minicore-sudotest)

# vfsmetatest(SP-9039F955, VFS uid/gid/mode + Chmod/Chown E2E 검증) -
# setuidtest/sudotest와 동일한 이유의 out-of-tree add_subdirectory -
# kmain.cpp의 TEMP 스폰 경로로 이름("vfsmetatest")이 매치돼 부팅 시
# 자동 실행된다(minicore/vfsmetatest/main.cpp 상단 주석 참고).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/vfsmetatest ${CMAKE_CURRENT_BINARY_DIR}/minicore-vfsmetatest)

# createusertest(DC-CC83F7BE, authmgr CreateUser 권한 검사 커널 중개
# 방식 E2E 검증) - 위 테스트들과 동일한 이유의 out-of-tree
# add_subdirectory - kmain.cpp의 TEMP 스폰 경로로 이름
# ("createusertest")이 매치돼 부팅 시 자동 실행된다
# (minicore/createusertest/main.cpp 상단 주석 참고).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/createusertest ${CMAKE_CURRENT_BINARY_DIR}/minicore-createusertest)

# lrutest(PN-B6DB692C, UserRecordCache LRU 축출 E2E 검증) - 위
# 테스트들과 동일한 이유의 out-of-tree add_subdirectory - kmain.cpp의
# TEMP 스폰 경로로 이름("lrutest")이 매치돼 부팅 시 자동 실행된다
# (minicore/lrutest/main.cpp 상단 주석 참고).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/lrutest ${CMAKE_CURRENT_BINARY_DIR}/minicore-lrutest)

# exittest/exitwaiter(PN-5EDE3C96 항목1, Process::exitCode 배선 E2E
# 검증) - sockinherit/sockinheritchild와 동일한 이유(exitwaiter가 진짜
# SpawnProcess로 exittest를 직접 스폰하므로 둘 다 필요) - kmain.cpp의
# TEMP 스폰 경로는 exitwaiter만 이름으로 매치한다(minicore/exitwaiter/
# main.cpp 상단 주석 참고).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/exittest ${CMAKE_CURRENT_BINARY_DIR}/minicore-exittest)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../minicore/exitwaiter ${CMAKE_CURRENT_BINARY_DIR}/minicore-exitwaiter)
