# minicore/devmgr - 장치 자동 인식/핫플러그 프레임워크의 서비스
# (SP-9DD4F3EA §6, PN-BD9AAE2F). **[전환, 2026-09-20, SP-43331889/
# QU-23B339AB/QU-ECEE5990]** 더 이상 별도 유저랜드 ELF가 아니다 -
# devmgr이 커널에 완전 흡수되며(Process 없는 순수 커널 Task) 이
# 디렉터리는 이제 minicore_kernel에 직접 링크되는 OBJECT 라이브러리를
# 만든다(userland/ 빌드 트리에서는 더 이상 이 서브디렉터리를 구성하지
# 않는다 - userland/CMakeLists.txt 참고). `crt0.S`(SysV 진입 스택
# 변환)는 ELF 로드 자체가 없어져 더 이상 안 쓴다 - 소스 파일은
# 그대로 남겨 두되(과거 유저랜드 시절 참고용) 빌드 대상에서 뺀다.
add_library(minicore_devmgr OBJECT main.cpp)

# minicore/kernel의 헤더(pnp.h/task.h 등)를 이 디렉터리 밖에서도
# 찾을 수 있게 명시적으로 추가한다 - minicore_kernel 자신은 전부
# 같은 디렉터리 안에서 quoted-include로 서로를 찾아 이 include
# 디렉터리가 필요 없었지만, devmgr은 물리적으로 다른 디렉터리에
# 있어 필요하다(devmgr_service.h는 같은 디렉터리라 그대로 찾아짐).
target_include_directories(minicore_devmgr PRIVATE ${CMAKE_SOURCE_DIR}/minicore/kernel)
target_link_libraries(minicore_devmgr PRIVATE libkenv)
