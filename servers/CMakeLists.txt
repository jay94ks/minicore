# 시스템 서비스 오케스트레이션 (직접 구현, ADR-006/007/008).
add_subdirectory(procsrv)
add_subdirectory(vfs)
add_subdirectory(fs)
add_subdirectory(netsrv)
add_subdirectory(devmgr)
add_subdirectory(drivers)
add_subdirectory(cfgsrv)
add_subdirectory(pipesrv)
add_subdirectory(login)
add_subdirectory(svcmgr)

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
              --service=pipesrv=$<TARGET_FILE:minicore_pipesrv_elf>
              --service=netsrv=$<TARGET_FILE:minicore_netsrv_elf>
              --service=shell=$<TARGET_FILE:minicore_shell_elf>
              --service=procsrv=$<TARGET_FILE:minicore_procsrv_elf>
              --service=login=$<TARGET_FILE:minicore_login_elf>
              --service=musl-hello=$<TARGET_FILE:minicore_musl_hello_elf>
              # docs/plan/musl-userland-porting.md §M51 — 익명 파이프+
              # dup2() 검증용 실제 musl 프로그램. pipesrv(위)+procsrv
              # 둘 다 핸들이 필요하다.
              --service=pipe-test=$<TARGET_FILE:minicore_pipe_test_elf>
              --service=user-service-delegate-test=$<TARGET_FILE:minicore_user_service_delegate_test_elf>
              --service=svcmgr=$<TARGET_FILE:minicore_svcmgr_elf>
              --service=svcmgr-ctl-test=$<TARGET_FILE:minicore_svcmgr_ctl_test_elf>
              # user-service-manager.md §M43 — 계정별 유저 서비스
              # 위임(ADR-218) grant 검증용 최소 프로그램. login이
              # test/root 두 계정을 로그인시킨 뒤(그 로그인 이벤트가
              # procsrv의 큐에 쌓인다), svcmgr의 로그인 감시 스레드가
              # 그 이벤트들을 실제로 소비하기 **전에**(svcmgr 자신이
              # 아직 부팅 중이라 아직 소비를 시작하지 않았다) 이
              # 프로그램이 두 계정의 위임을 등록해 둔다 — musl-hello
              # 뒤·svcmgr 앞이라는 순서 자체가 이 타이밍을 보장한다
              # (userland/user-service-delegate-test/main.c 참고).
              # user-service-manager.md §M40, ADR-192 §결정2 — svcmgr가
              # "모든 커널 서버 기동 후 마지막으로" 스폰되는 보장은
              # --service= 목록에서 이게 **가장 마지막 줄**이라는
              # 사실 자체에서 나온다(init/initrun/main.cpp::spawn_visit
              # 이 cpio 아카이브 순서 그대로 스폰한다, tools/mkbootdisk.py
              # 는 --service= 인자 순서를 그대로 archive에 옮길 뿐 별도
              # 정렬을 하지 않는다) — --depends=는 이 순서를 강제하지
              # 않는다(핸들 상속에만 쓰인다, 아래 실행 중 발견 참고).
              # svcmgr의 --depends=는 procsrv 하나만 둔다 — 처음엔
              # ADR-196 §결정1의 "의존관계는 기존 커널 서버 전체"라는
              # 문구를 문자 그대로 커널 서버 15개를 다 나열했다가 두
              # 가지를 실제로 겪었다: (1) MC_MAX_SPAWN_INHERITED_HANDLES
              # (=4)를 넘는 이름은 애초에 핸들을 못 받는다(조용히
              # 무시됨, 오류 아님) (2) initrun의 depends= 파싱 버퍼
              # (char deps_buf[96], init/initrun/main.cpp)보다 그 문자열이
              # 길어(15개 이름+콤마=110자 이상) copy_span_to_cstr가 그
              # 자리에서 실패해 depends= 전체가 통째로 무시돼(procsrv
              # 핸들조차 못 받음) svcmgr의 self_register가 엉뚱한
              # (존재하지 않는) 핸들로 IPC를 걸어 즉시 실패했다 — 실제
              # 필요한 건 순서 보장(이미 위 --service= 위치로 충족)이
              # 아니라 필요한 핸들뿐이었다. M41(user-service-manager.md
              # §M41)이 cfgsrv도 실제로 쓰게 돼(@global/system/services
              # 테이블) procsrv,cfgsrv 둘로 늘렸다 — 여전히
              # MC_MAX_SPAWN_INHERITED_HANDLES(=4)와 96바이트 파싱
              # 버퍼 둘 다 안전하게 여유가 있다.
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
              # pipe-test는 vfs를 실제로 쓰지 않지만, syscall_shim.c의
              # MC_PROCSRV_HANDLE(=3)이 musl-hello와 똑같이 "vfs가
              # 먼저 오는" 순서를 하드코딩 전제로 삼고 있어(모든 musl
              # 프로그램이 공유하는 단일 syscall_shim.c 빌드이므로
              # 프로그램마다 다른 handle 번호를 못 쓴다) vfs를 그대로
              # 첫 의존으로 둔다 — 실제 사용은 procsrv(fork/wait)+
              # pipesrv(pipe)뿐이다.
              --depends=pipe-test:vfs,procsrv,pipesrv
              --depends=user-service-delegate-test:cfgsrv
              --depends=svcmgr:procsrv,cfgsrv
              # M42(user-service-manager.md §M42) — "새 컨트롤
              # 클라이언트"(계획 문서가 착수 시점에 확정하라고 남긴
              # 것)를 별도 최소 테스트 프로그램으로 구현했다 — svcmgr
              # 뒤에 스폰되고 svcmgr/cfgsrv 둘 다에 핸들이 필요하다.
              --depends=svcmgr-ctl-test:svcmgr,cfgsrv
              --linux-abi-stack=musl-hello
              --linux-abi-stack=pipe-test
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
              minicore_pipesrv_elf
              minicore_netsrv_elf minicore_shell_elf minicore_procsrv_elf
              minicore_devmgr_elf minicore_drv_ps2_elf minicore_drv_usb_elf
              minicore_drv_virtio_blk_elf minicore_drv_virtio_net_elf
              minicore_fat32_elf minicore_ext4_elf
              minicore_drv_console_elf minicore_login_elf
              minicore_musl_hello_elf
              minicore_pipe_test_elf
              minicore_user_service_delegate_test_elf
              minicore_svcmgr_elf
              minicore_svcmgr_ctl_test_elf
              ${CMAKE_SOURCE_DIR}/tools/mkbootdisk.py
      COMMENT "Packaging bootdisk.img (cpio newc) from memfs+devmgr+ps2+console+usb+virtio-blk+virtio-net+fat32+ext4+vfs+cfgsrv+pipesrv+netsrv+shell+procsrv+login+musl-hello+pipe-test+user-service-delegate-test+svcmgr+svcmgr-ctl-test"
      VERBATIM
  )
  add_custom_target(minicore_bootdisk_image DEPENDS ${MINICORE_BOOTDISK_IMAGE})
  set_property(GLOBAL PROPERTY MINICORE_BOOTDISK_IMAGE_PATH ${MINICORE_BOOTDISK_IMAGE})
endif()
