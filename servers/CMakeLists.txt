# 시스템 서비스 오케스트레이션 (직접 구현, ADR-006/007/008).
add_subdirectory(procsrv)
add_subdirectory(vfs)
add_subdirectory(fs)
add_subdirectory(netsrv)
add_subdirectory(devmgr)
add_subdirectory(drivers)

# 부트 디스크(bootdisk.img) 조립 — M13(system-servers-bringup.md)부터
# memfs/vfs/procsrv 세 서비스를 함께 담는다(M12는 procsrv 하나만이라
# servers/procsrv/CMakeLists.txt에 있었다 — 이제 여기서만 셋 다 참조
# 가능해서 옮겼다). M14부터 devmgr/ps2/usb 세 개를 더 담는다.
# --depends는 ADR-152의 스폰 시점 캐패빌리티 주입과 맞물린다 —
# initrun이 lib/*.ini의 depends= 키를 보고 그 의존 서비스의 endpoint
# 프록시를 물려준다. --trusted는 ADR-147/154/156의 trusted 판정과
# 맞물린다(devmgr/ps2/usb는 실제 하드웨어를 직접 다뤄야 한다). 나열
# 순서(=cpio 기록 순서=파일명순=실행순, ADR-131 §결정5)가 곧 의존성
# 해결 순서라 memfs→vfs→procsrv→devmgr→ps2→usb로 고정한다(usb는
# devmgr가 이미 PCI를 열거해 둔 뒤에 register_driver를 호출해야
# 하므로 devmgr보다 뒤).
if(MINICORE_ARCH STREQUAL "x86_64")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  set(MINICORE_BOOTDISK_IMAGE ${CMAKE_CURRENT_BINARY_DIR}/bootdisk.img)
  add_custom_command(
      OUTPUT ${MINICORE_BOOTDISK_IMAGE}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mkbootdisk.py
              ${MINICORE_BOOTDISK_IMAGE}
              --service=memfs=$<TARGET_FILE:minicore_memfs_elf>
              --service=vfs=$<TARGET_FILE:minicore_vfs_elf>
              --service=procsrv=$<TARGET_FILE:minicore_procsrv_elf>
              --service=devmgr=$<TARGET_FILE:minicore_devmgr_elf>
              --service=ps2=$<TARGET_FILE:minicore_drv_ps2_elf>
              --service=usb=$<TARGET_FILE:minicore_drv_usb_elf>
              --depends=vfs:memfs
              --depends=procsrv:vfs
              --depends=usb:devmgr
              --trusted=devmgr
              --trusted=ps2
              --trusted=usb
      DEPENDS minicore_memfs_elf minicore_vfs_elf minicore_procsrv_elf
              minicore_devmgr_elf minicore_drv_ps2_elf minicore_drv_usb_elf
              ${CMAKE_SOURCE_DIR}/tools/mkbootdisk.py
      COMMENT "Packaging bootdisk.img (cpio newc) from memfs+vfs+procsrv+devmgr+ps2+usb"
      VERBATIM
  )
  add_custom_target(minicore_bootdisk_image DEPENDS ${MINICORE_BOOTDISK_IMAGE})
  set_property(GLOBAL PROPERTY MINICORE_BOOTDISK_IMAGE_PATH ${MINICORE_BOOTDISK_IMAGE})
endif()
