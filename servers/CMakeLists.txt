# 시스템 서비스 오케스트레이션 (직접 구현, ADR-006/007/008).
add_subdirectory(procsrv)
add_subdirectory(vfs)
add_subdirectory(fs)
add_subdirectory(netsrv)
add_subdirectory(devmgr)
add_subdirectory(drivers)
add_subdirectory(cfgsrv)
add_subdirectory(login)

# 부트 디스크(bootdisk.img) 조립 — M13(system-servers-bringup.md)부터
# memfs/vfs/procsrv 세 서비스를 함께 담는다(M12는 procsrv 하나만이라
# servers/procsrv/CMakeLists.txt에 있었다 — 이제 여기서만 셋 다 참조
# 가능해서 옮겼다). M14부터 devmgr/ps2/usb, M15부터 virtio-blk,
# M16부터 fat32/ext4, M17부터 console/login, M19부터 cfgsrv, M20부터
# userland/shell, M25부터 virtio-net/netsrv을 더 담는다. --depends는
# ADR-152의 스폰 시점 캐패빌리티 주입과 맞물린다 — initrun이
# lib/*.ini의 depends= 키를 보고 그 의존 서비스(들)의 endpoint
# 프록시를 물려준다(M16부터 콤마로 여러 개 가능, init/initrun/main.cpp
# 참고). --trusted는 ADR-147/154/156/164/169의 trusted 판정과
# 맞물린다(devmgr/ps2/usb/virtio-blk/virtio-net/fat32/ext4/console은
# 실제 하드웨어를 직접 다뤄야 하고, cfgsrv는 ADR-063/074의 신뢰
# 프로세스 보호 대상이다) — netsrv는 하드웨어를 직접 만지지 않고
# virtio-net에게 IPC로만 위임하므로 trusted가 아니다(vfs와 같은
# 정신). 나열 순서(=cpio 기록 순서=파일명순=실행순, ADR-131 §결정5)
# 가 곧 의존성 해결 순서다 — shell은 vfs/console/ps2에 의존해 부팅
# 시 바로 스폰되지만 자기 handle 1에서 procsrv의 OP_START
# (security-model.md ADR-171)를 기다리며 블록한다. procsrv는
# vfs/cfgsrv/shell 셋 다에 의존해(로그인 성공 시 셸에 OP_START를
# 보내려면 그 핸들이 필요) login은 console/ps2/procsrv 전부에
# 의존하므로 전체 순서는 memfs→devmgr→ps2→console→usb→virtio-blk→
# virtio-net→fat32→ext4→vfs→cfgsrv→netsrv→shell→procsrv→login→
# musl-hello. musl-hello는 M31(real-libc-syscall-layer.md §M31)이
# vfs 의존을 추가한 것 — --linux-abi-stack=(mkbootdisk.py 신규,
# tools/mkbootdisk.py 참고)로 표시된 유일한 서비스라 initrun이
# musl의 crt_arch.h가 요구하는 Linux ABI 초기 스택으로 스폰한다
# (다른 모든 서비스는 이 커널의 기존 arg0 관례를 그대로 쓴다).
# M32(real-libc-syscall-layer.md §M32) — musl-hello가 진짜
# fork()+execve()+waitpid()로 procsrv에 pid를 등록/조회하려면
# procsrv의 endpoint도 물려받아야 해서 procsrv를 두 번째 의존으로
# 추가했다(handle 순서는 libc/sysdeps/minicore/syscall_shim.c::
# MC_PROCSRV_HANDLE 참고). execve()가 실행할 "다른 이미지"
# (userland/musl-exec-target)는 이 목록에 없다 — initrun이 부팅 시
# 스폰하는 서비스가 아니라, procsrv가 VFS(memfs)에 미리 써 두고
# musl-hello가 런타임에 경로로 여는 대상이다(servers/procsrv/
# CMakeLists.txt의 tools/bin2c.py 배선 참고).
if(MINICORE_ARCH STREQUAL "x86_64")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  set(MINICORE_BOOTDISK_IMAGE ${CMAKE_CURRENT_BINARY_DIR}/bootdisk.img)
  add_custom_command(
      OUTPUT ${MINICORE_BOOTDISK_IMAGE}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mkbootdisk.py
              ${MINICORE_BOOTDISK_IMAGE}
              --service=memfs=$<TARGET_FILE:minicore_memfs_elf>
              --service=devmgr=$<TARGET_FILE:minicore_devmgr_elf>
              --service=ps2=$<TARGET_FILE:minicore_drv_ps2_elf>
              --service=console=$<TARGET_FILE:minicore_drv_console_elf>
              --service=usb=$<TARGET_FILE:minicore_drv_usb_elf>
              --service=virtio-blk=$<TARGET_FILE:minicore_drv_virtio_blk_elf>
              --service=virtio-net=$<TARGET_FILE:minicore_drv_virtio_net_elf>
              --service=fat32=$<TARGET_FILE:minicore_fat32_elf>
              --service=ext4=$<TARGET_FILE:minicore_ext4_elf>
              --service=vfs=$<TARGET_FILE:minicore_vfs_elf>
              --service=cfgsrv=$<TARGET_FILE:minicore_cfgsrv_elf>
              --service=netsrv=$<TARGET_FILE:minicore_netsrv_elf>
              --service=shell=$<TARGET_FILE:minicore_shell_elf>
              --service=procsrv=$<TARGET_FILE:minicore_procsrv_elf>
              --service=login=$<TARGET_FILE:minicore_login_elf>
              --service=musl-hello=$<TARGET_FILE:minicore_musl_hello_elf>
              --depends=vfs:memfs,fat32,ext4
              --depends=cfgsrv:vfs
              --depends=netsrv:virtio-net
              --depends=shell:vfs,console,ps2
              --depends=procsrv:vfs,cfgsrv,shell
              --depends=usb:devmgr
              --depends=virtio-blk:devmgr
              --depends=virtio-net:devmgr
              --depends=fat32:devmgr
              --depends=ext4:devmgr
              --depends=login:console,ps2,procsrv
              --depends=musl-hello:vfs,procsrv
              --linux-abi-stack=musl-hello
              --trusted=devmgr
              --trusted=ps2
              --trusted=usb
              --trusted=virtio-blk
              --trusted=virtio-net
              --trusted=fat32
              --trusted=ext4
              --trusted=console
              --trusted=cfgsrv
      DEPENDS minicore_memfs_elf minicore_vfs_elf minicore_cfgsrv_elf
              minicore_netsrv_elf minicore_shell_elf minicore_procsrv_elf
              minicore_devmgr_elf minicore_drv_ps2_elf minicore_drv_usb_elf
              minicore_drv_virtio_blk_elf minicore_drv_virtio_net_elf
              minicore_fat32_elf minicore_ext4_elf
              minicore_drv_console_elf minicore_login_elf
              minicore_musl_hello_elf
              ${CMAKE_SOURCE_DIR}/tools/mkbootdisk.py
      COMMENT "Packaging bootdisk.img (cpio newc) from memfs+devmgr+ps2+console+usb+virtio-blk+virtio-net+fat32+ext4+vfs+cfgsrv+netsrv+shell+procsrv+login+musl-hello"
      VERBATIM
  )
  add_custom_target(minicore_bootdisk_image DEPENDS ${MINICORE_BOOTDISK_IMAGE})
  set_property(GLOBAL PROPERTY MINICORE_BOOTDISK_IMAGE_PATH ${MINICORE_BOOTDISK_IMAGE})
endif()
