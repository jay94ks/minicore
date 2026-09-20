# minicore/fs - VFS 마운트 지점 실질 처리 서비스(SP-8B6B8D25 §4-A,
# PN-452FF696). **[전환, 2026-09-20, SP-43331889/QU-5FC58B06]** 더
# 이상 별도 유저랜드 ELF가 아니다 - fs가 커널에 완전 흡수되며(Process
# 없는 순수 커널 Task) 이 디렉터리는 이제 minicore_kernel에 직접
# 링크되는 OBJECT 라이브러리를 만든다(userland/ 빌드 트리에서는 더
# 이상 이 서브디렉터리를 구성하지 않는다 - userland/CMakeLists.txt
# 참고, minicore/devmgr/CMakeLists.txt와 동일한 패턴).
add_library(minicore_fs OBJECT main.cpp ahci.cpp)

# minicore/kernel의 헤더(pnp.h/task.h 등)를 이 디렉터리 밖에서도
# 찾을 수 있게 명시적으로 추가한다(devmgr/CMakeLists.txt와 동일한
# 이유 - fs는 물리적으로 다른 디렉터리에 있다, fs_service.h/ahci.h/
# block_device.h는 같은 디렉터리라 그대로 찾아짐).
target_include_directories(minicore_fs PRIVATE ${CMAKE_SOURCE_DIR}/minicore/kernel)
target_link_libraries(minicore_fs PRIVATE libkenv)
