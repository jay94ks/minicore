# 실행 계획: 시스템 서버 로드맵 (M12~M20) — procsrv부터 로그인 후 셸까지

**관련 결정**: [foundations.md](../design/foundations.md)(ADR-005·008·
**132**), [kernel-ipc-objects.md](../design/kernel-ipc-objects.md),
[filesystem.md](../design/filesystem.md)(ADR-018·044~050·058~059·065·
080·099~103·**128**), [boot-and-drivers.md](../design/boot-and-drivers.md)
(ADR-038~043·056·057·**129·130·131**), [security-model.md](../design/security-model.md),
[registry-decisions.md](../design/registry-decisions.md)(ADR-060~064)

**선행 완료 전제**: [kernel-bootstrap.md](kernel-bootstrap.md)(M1~M8)과
[smp-fpu-bringup.md](smp-fpu-bringup.md)(M9~M11b)이 끝나 있어야 한다 —
이 계획은 initrun이 뜬 이후, "실제로 쓸모 있는 다중 프로세스 시스템"을
만드는 단계다.

## 배경과 성격

이 계획의 마일스톤 대부분은 **새 아키텍처 결정이 필요 없다** —
`servers/`(repo-layout.md)가 이미 나열한 서버들(procsrv/vfs/fs/devmgr/
cfgsrv/drivers)과, `docs/design/`에 이미 방대하게 확정된 ADR들
(IPC·객체 모델, 파일시스템, PCIe/드라이버 로드맵, 보안 모델, 설정
리포지터리)을 **순서대로 구현·검증**하는 작업이다. 그래서 각
마일스톤은 kernel-bootstrap.md M1~M8과 같은 형식(구현 대상 + 관련
기존 ADR/스펙 + QEMU 검증 목표)으로만 적는다 — 세부 프로토콜은
`docs/spec/`에 각 마일스톤 착수 시점에 개별 작성한다(ADR-043이 이미
이 방식을 명시한 선례).

이 계획을 세우면서 예외가 여섯 생겼다 — 전부 이번에 새 ADR로 미리
해소해뒀다:

- **ADR-132**(foundations.md) — libc 포팅과 별개로, minicore 자체
  syscall/서버 프로토콜에 1:1(또는 최소 확장) 대응하는 순수 C+어셈블러
  라이브러리 **`libmc`**를 새로 둔다. libc는 `libmc` 위의 클라이언트가
  되고(ADR-008이 이미 예고한 "libc는 클라이언트" 원칙의 구체화),
  minicore 네이티브 서버(procsrv 등)도 libc 없이 `libk`+`libmc`만
  링크한다 — 프로토콜 클라이언트 구현이 libc 포팅 글루/각 서버에
  중복되지 않고 한 곳에만 있다. 아래 **libmc 교차 트랙** 참고.

- **ADR-131**(boot-and-drivers.md, OPEN-29·OPEN-50 해소) — initrun은
  고정 이름 하나를 spawn하는 게 아니라, **커널이 initrd의 `disk.cfg`
  엔트리(OS 설치 시점 구성값, 지금은 mkinitrd.py가 대신 채움, 없거나
  장치가 안 보이면 initrun 자신의 임베디드 PCIe 폴백 스캔)로 전달한
  부트 디바이스 서술자로, 그 장치를 자신에게 내장된 최소 virtio-blk
  클라이언트+**cpio(newc)** 리더로 직접 마운트**하고, 아카이브 안
  `lib/NNN-이름.ini`(VFS 경로가 아니라 아카이브 내부 상대 경로,
  파일명순=실행순, `exec`/`args`+레지스트리 대체용 폴백 설정) 파일들로
  `bin/`의 procsrv를 포함한 서비스 바이너리들을 찾아 **각자의 초기화
  완료를 기다리며** 순서대로
  `sys_process_spawn`(원본 ELF 바이트를 직접 넘기는 형태로 일반화)한다.
  전부 끝나면 **systemd류 초기 프로세스를 마지막으로 실행시키고
  initrun 스스로는 사라진다**(그 정체성과 절차는 OPEN-51로 남음).
  devmgr/virtio-blk 서버(M14~M15)의 "진짜" 구현과는 의도적으로
  별개인 부트스트랩 전용 코드이며, cpio 부트 디바이스는 Linux
  initramfs처럼 일회성이라 M16의 FAT32 FS 서버(`/boot/uefi` ESP용)와도
  무관하다.
- **ADR-128**(filesystem.md) — M16에 ext4를 추가하려면 FS마다 다른
  "파일 신원"을 가리키는 공통 개념이 있어야 하는데(공유 모드 충돌
  판별 등, ADR-101이 암묵적으로 요구해왔다) 지금까지 정의된 적이
  없었다 — `fs_node_id`라는 불투명 식별자로 새로 정의했다.
- **ADR-129**(boot-and-drivers.md) — ext4를 로드맵에 추가하면서
  구현 범위(extent만, 저널 재생 없음, htree 미사용)를 명시적으로
  좁혔다 — 안 그러면 M16 하나가 별도 프로젝트급이 된다.
- **ADR-130**(boot-and-drivers.md) — M14에 PS/2·USB를 추가하려니
  기존 ADR-038~041이 전부 PCIe 전용이라 USB(중첩 버스)와 PS/2(버스
  아님, 고정 레거시 프로브)를 담을 자리가 없었다 — 입력 장치
  클래스를 새로 정의했다.

## libmc 교차 트랙 (M12~M20 전 구간)

[ADR-132](../design/foundations.md)로 확정한 `libmc`는 한 마일스톤의
산출물이 아니라 **이 계획 전체에 걸쳐 그때그때 필요한 클라이언트가
쌓이는 교차 트랙**이다 — 새 서버 프로토콜이 생기는 마일스톤마다
그 서버를 부르는 `mc_*` 클라이언트 함수도 같은 마일스톤에서 함께
작성한다(서버 구현과 그 서버의 표준 클라이언트를 분리해서 나중에
몰아 만들지 않는다 — 프로토콜을 막 확정한 시점에 클라이언트도 같이
써야 서버·클라이언트가 실제로 들어맞는지 바로 검증된다):

| 마일스톤 | libmc에 추가되는 것 |
|---|---|
| M12 | syscall 1:1 래퍼 전체(`mc_ipc_*`, `mc_handle_close`, ADR-131의 `mc_process_spawn`) + `_start` 진입점(§ADR-132 결정3) + procsrv 클라이언트(`mc_fork`/`mc_exec`/fd 프로토콜). initrun 자신의 임베디드 virtio-blk/cpio 리더+PCIe 폴백 스캔은 부트스트랩 전용이라 `libmc` 범위 밖(ADR-131 §근거) |
| M13 | VFS 클라이언트(`mc_open`/`mc_stat`) + FS 서버 공통 프로토콜 클라이언트(`mc_read`/`mc_write`/`mc_close`, ADR-101 공유모드/잠금 포함) |
| M14 | devmgr 등록 클라이언트(드라이버가 자신을 등록하는 쪽, ADR-041) |
| M17 | 콘솔/로그인 클라이언트(`mc_login` 등, procsrv §8 프로토콜) |
| M18 | 신뢰 위임·su/sudo·jail 관련 호출 클라이언트 |
| M19 | cfgsrv 클라이언트(`mc_cfg_get`/`mc_cfg_set`) |
| M20 | `libc/sysdeps/minicore`가 여기까지 쌓인 `libmc`를 처음으로 실제 소비 — 이 시점 이전에는 `libmc`만 있고 포팅된 libc는 아직 없다 |

각 마일스톤의 §구현에 "그 서버의 libmc 클라이언트도 함께 작성"이
암묵적으로 포함된다고 보고, 아래 마일스톤 절에서 매번 반복해 적지
않는다 — 이 표가 그 대응표다. `libmc` 자체의 세부 ABI는 M12 착수
시점에 `docs/spec/`로 별도 작성한다(ADR-132 §영향).

## M12. 부트 디바이스 마운트 + procsrv 골격 — 프로세스 테이블·fork/exec·계정 모델·fd 진실 공급원

- **선행 결정**: **ADR-131**(boot-and-drivers.md, OPEN-29·OPEN-50
  해소 완료) — initrun이 부트 디바이스(cpio 아카이브)를 직접 마운트해
  아카이브 안 `lib/NNN-이름.ini`(VFS 경로가 아니라 아카이브 내부
  상대 경로, 발견 순서=파일명순, `exec`/`args`+레지스트리 대체용
  폴백 설정) 파일들로 `bin/`의 서비스를 찾아, 각자의 초기화 완료를
  기다리며 기동하는 전체 절차가 이미 확정돼 있다. 이 마일스톤은
  그대로 구현만 하면 된다 — 다만 **OPEN-51**(모든 서비스 기동 후
  마지막에 실행하는 "systemd류 초기 프로세스"의 정체성과 절차)은
  착수 시점에 먼저 좁혀야 한다.
- **구현**: 두 갈래다.
  1. **initrun의 부트스트랩 절차**(ADR-131) — `boot_info.
     boot_device_descriptor` 파싱(없거나 장치가 안 보이면 임베디드
     PCIe 폴백 스캔), 임베디드 최소 virtio-blk 클라이언트(블로킹,
     읽기 전용), 임베디드 최소 **cpio(newc)** 읽기전용 파서 + INI
     파서, 아카이브 안 `lib/*.ini`를 파일명순으로 나열해 각각의
     `exec=`/`args=`로 `sys_process_spawn(elf_data, elf_size, argv,
     grant_trusted)`(신설 커널 syscall)를 호출하고 그 서비스의
     초기화 완료 신호를 기다린 뒤 다음으로 넘어간다(신호 프로토콜
     자체는 OPEN-52로 분리된 별도 design 대상이라, 이 마일스톤은
     그 design이 나올 때까지 쓸 최소 임시 방편 — 예를 들어 정해진
     타임아웃이나 단순 notification 한 번 — 으로 진행하고, 정식
     프로토콜이 나오면 교체한다). 나머지
     key=value는 원본 그대로 새 프로세스에게
     `boot_info.config_blob_addr/size`로 매핑해 전달한다. 전부 끝나면
     OPEN-51에서 정할 "systemd류 초기 프로세스"를 마지막으로 실행시키고
     initrun 자신은 종료한다. `tools/mkinitrd.py`가 `disk.cfg` 엔트리를
     채우도록 확장하고, 새 `tools/mkbootdisk.py`로 `bin/procsrv` +
     `lib/000-procsrv.ini`를 담은 **cpio** 부트 디스크
     이미지를 만들어 `tools/run-qemu.sh`가 virtio-blk로 붙이도록
     확장한다.
  2. **procsrv 자체**([procsrv.md](../spec/procsrv.md)) — 프로세스
     테이블·fork/exec 시퀀스(ADR-016 COW 활용)·fd 진실 공급원
     프로토콜, 계정 모델([security-model.md](../design/security-model.md)
     ADR-079 — uid/gid 발급, ROOT/Supervisor 식별) 저장소까지.
     로그인/session_program 프로토콜 자체(§procsrv.md)는 이
     마일스톤에서 **프로토콜 골격만** 만들고 실제 콘솔 연동은 M17로
     미룬다(procsrv.md 자신이 이미 이렇게 scope함).
- **목표(QEMU 검증)**: initrun이 `boot_info`로 받은 서술자로 부트
  디스크(virtio-blk 위 cpio)를 마운트하고, 그 안에서 procsrv를
  찾아 초기화 완료를 기다리며 `sys_process_spawn`으로 띄우는 것과,
  procsrv가 **자기 자신을 fork/exec**해 실제 두 번째 완전한 유저
  프로세스를 만들어내는 것을 로그로 확인한다 — M4~M8까지는 커널이
  직접 만든 스레드/프로세스뿐,
  실제 디스크 I/O를 거친 프로세스 생성은 이번이 처음이다.

## M13. VFS + memfs — fd 라우팅 실동작 + 최초 파일시스템

- **구현**: [filesystem.md](../design/filesystem.md) ADR-018(fd→서버
  캐패빌리티/핸들 매핑)을 M12의 fd 프로토콜과 실제로 연결하고,
  마운트 네임스페이스 기초(ADR-044/045)를 갖춘 `vfs` 서버를 만든다.
  최초 파일시스템은 순수 인메모리인 `fs/memfs`(repo-layout.md가
  "최초 구현 대상"으로 이미 지정) — 블록 드라이버 없이 VFS 프로토콜
  자체를 검증하는 게 목적이다.
- **목표**: M12의 두 번째 프로세스가 VFS 경유로 memfs에 파일을 쓰고
  다시 읽어 내용이 일치함을 확인한다.

## M14. devmgr + PCIe 버스 열거 + 입력 장치(PS/2·USB)

- **선행 작업(먼저 구현)**: **ADR-154**(boot-and-drivers.md, OPEN-58
  해소) — TSS IOPB를 코어당 전역 1회성에서 스레드별 활성 I/O 포트
  범위(`sys_io_activate`/`sys_io_deactivate` + 컨텍스트 스위치 시
  diff 재프로그래밍)로 재설계한 것을 실제로 구현한다. M14가 devmgr
  하나가 아니라 PS/2·USB 등 **여러** 드라이버가 각자 다른 I/O 포트
  범위를 동시에 필요로 하는 첫 마일스톤이라, ADR-147의 "전역 1회성
  IOPB" 단순화가 여기서부터 실제로 부족해진다 — 드라이버를 만들기
  전에 먼저 고쳐야 한다. **ADR-155**(kernel-ipc-objects.md, OPEN-59
  해소) §1(커널/커널스레드 수신자용 `pages[]` cross-address-space
  번역)도 구현 비용이 작아 이 시점에 함께 넣는다(§2의 유저 프로세스
  대상 공유 매핑 경로는 OPEN-61이 먼저 정해져야 해서 M16 전후로
  미룬다).
- **구현**: [pcie.md](../spec/pcie.md), boot-and-drivers.md
  ADR-038~041(ECAM 설정공간 접근, devmgr 버스 열거, 핫플러그, 동적
  드라이버 등록) — PCIe 열거 자체는 아직 스토리지/NIC/GPU 드라이버를
  만들지 않는다(열거만). 여기에 새 **ADR-130**(입력 장치 로드맵)에
  따라 두 입력 드라이버를 실제로 만든다:
  - `servers/drivers/ps2` — devmgr 열거를 거치지 않는 고정 레거시
    프로브(`0x60`/`0x64`), M17(콘솔/로그인)이 필요로 하는 키보드
    입력의 1순위 경로.
  - `servers/drivers/usb` — xHCI 컨트롤러(PCIe 장치로 발견)를
    devmgr가 ADR-130이 새로 정의한 "중첩 버스 열거"로 다루고, 그
    위에 최소 HID 클래스 드라이버를 얹는다.
- **목표**: QEMU가 붙인 `virtio-blk`/`virtio-net`/`virtio-gpu` 각
  장치의 벤더/클래스 ID를 devmgr가 읽어 로그로 남긴다. PS/2
  드라이버는 QEMU 키 입력을 스캔코드로 수신해 로그로 남기고, USB HID
  드라이버는 devmgr가 열거한 xHCI 포트 위의 HID 장치(QEMU
  `-device usb-kbd` 등)를 인식했음을 로그로 남긴다.

## M15. virtio-blk 드라이버 — 첫 실제 유저 드라이버

- **구현**: boot-and-drivers.md ADR-043의 1순위(스토리지 클래스).
  `servers/drivers/`에 virtio-blk 드라이버 신설, M14의 devmgr가
  이 드라이버를 자동 기동.
- **목표**: 알려진 패턴을 QEMU가 붙인 디스크 이미지에 블록 단위로
  쓰고 다시 읽어 일치함을 확인한다.

## M16. FAT32 + ext4 FS 서버 (ADR-057, ADR-128, ADR-129)

- **선행 결정**: [filesystem.md](../design/filesystem.md) ADR-128이
  FS 서버 공통 프로토콜의 파일 신원 개념(`fs_node_id`)을 이미
  정의했다 — FAT32는 첫 클러스터 번호로, ext4는 네이티브 inode
  번호를 그대로 써서 합성한다. 새로 결정할 것 없이 그대로 구현만
  하면 된다.
- **선행 작업(먼저 구현)**: **ADR-155**(kernel-ipc-objects.md,
  OPEN-59 해소) §2 — IPC `pages[]`를 유저 프로세스 수신자의
  주소공간에 참조 카운트+공유 매핑으로 전달하는 경로. M13의
  fs-protocol.md는 이 경로가 없어 경로/데이터를 전부 `regs[]`(최대
  32/16바이트)로 눌러 담는 임시 인코딩을 썼다 — 실제 FAT32/ext4
  파일은 그 한도를 훌쩍 넘으므로, 이 마일스톤 착수 시점에 먼저 이
  경로를 구현하고 fs-protocol.md를 `pages[]` 기반으로 교체해야 한다.
  매핑의 배치 위치(스레드별 고정 슬롯, `k_user_stack_top` 위쪽
  사다리)와 해제 시점(다음 `sys_recv` 직전 자동 해제)은 **ADR-159**
  (OPEN-61 해소)로 이미 설계 확정됨 — 이 마일스톤에서는 그 설계를
  그대로 구현만 하면 된다.
- **구현**:
  - `fs/fat32`(ADR-057, FAT32를 virtio-blk 검증 직후 착수하기로
    이미 확정) — M15의 블록 드라이버 위에 올리고 M13의 VFS에 마운트
    지점으로 연결한다.
  - `fs/ext4`(**ADR-129**) — FAT32 다음 순서. v1 범위는 ADR-129가
    이미 좁혀뒀다: extent 트리 기반 블록 매핑만, 저널(JBD2) 재생
    없음(clean 저널만 마운트, 없으면 읽기전용/거부), htree 인덱스는
    안 쓰고 디렉터리 블록 선형 스캔, 지원 안 하는
    `feature_incompat`/`feature_ro_compat` 비트가 있으면 마운트
    거부.
- **목표**: 호스트에서 미리 만든 FAT32 이미지와, **정상 unmount(clean
  저널)한 ext4 이미지** 양쪽에서 각각 파일을 마운트 후 VFS 경유로
  열어 내용을 읽어낸다. 두 FS가 서로 다른 `fs_node_id` 매핑 전략을
  쓰면서도 같은 VFS 프로토콜로 동시에 동작함을 확인하는 것이
  ADR-128 설계가 실제로 성립함을 보여주는 핵심 검증이다.

## M17. 콘솔/TTY 드라이버 + 로그인 흐름

- **구현**: boot-and-drivers.md ADR-111(콘솔/TTY 상세 프로토콜)과
  ADR-097/098(로그인 프롬프트 프로세스↔콘솔 드라이버 연결, 다중
  콘솔 기동). 이 콘솔은 M1의 디버그 시리얼 콘솔([debug-console.md](../spec/debug-console.md))과
  **완전히 별개**(그 스펙 §1이 이미 명시) — 여기서 처음으로 진짜
  유저용 콘솔이 생긴다. 로그인 프롬프트가 M12의 계정 저장소로
  인증한다.
- **주의**: 지금까지 `run-qemu.sh`는 `-display none`(순수 시리얼)을
  써 왔다 — 프레임버퍼/텍스트모드 콘솔을 실제로 보려면 `-display`
  옵션 처리를 이 마일스톤에서 opt-in으로 추가해야 한다(ADR-125가
  세운 "기본값 유지 + opt-in 환경변수" 패턴을 그대로 따른다).
- **목표**: QEMU 콘솔 화면에 로그인 프롬프트가 뜨고, M12 계정으로
  로그인에 성공한다.

## M18. 보안 모델: 신뢰 위임 체인 + su/sudo + jail/guest

- **구현**: security-model.md ADR-074/075(trusted 위임 체인), ADR-093
  (sudoers류 승인 정책), ADR-081/083(jail 격리, `/sys/proc`·`/sys/dev`
  가시성 필터링). 이미 M6/M7이 badge 전파 자체는 커널 스레드 데모로
  검증해 뒀으므로, 여기서는 **실제 별도 프로세스 두 개** 사이에서
  같은 메커니즘이 성립함을 확인하는 것이 핵심이다.
- **목표**: 일반 계정 프로세스가 `sudo`류 요청 → 정책 확인 → trusted
  프로세스로부터 권한 위임까지 성공하고, jail 프로세스는 격리된 VFS
  루트 밖 접근이 거부됨을 확인한다.

## M19. cfgsrv (설정 리포지터리)

- **구현**: [registry.md](../spec/registry.md), registry-decisions.md
  ADR-060~064(스키마/테이블 주소체계, 권한 모델, 전용 IPC 프로토콜,
  비밀 데이터 보호).
- **목표**: procsrv(또는 다른 서버)가 cfgsrv에 설정값을 쓰고 다시
  읽는 왕복이 권한 모델대로(허용/거부) 동작함을 확인한다.

## M20. libc/POSIX 최소 포팅 + 로그인 후 셸 — 이 계획의 최종 완료 기준

- **구현**: ADR-005/008(POSIX 계층 배치·범위)에 따라 `third_party/`에
  최소 libc 하나와 셸 하나를 서브모듈로 들이고, `libc/sysdeps/minicore/`
  글루로 `syscall`/VFS(M13)/procsrv(M12) IPC에 연결한다(repo-layout.md
  §libc/userland).
- **목표**: M17의 로그인 프롬프트를 통과하면 셸 프롬프트가 뜨고,
  `ls`/`cat` 같은 기본 명령으로 M13(memfs)·M16(FAT32/ext4) 위의
  파일을 조회할 수 있다 — **이 계획 전체의 최종 완료 기준**.

## 범위 밖 (M20 이후로 미룸)

- **netsrv/virtio-net**(소켓·프로토콜 스택) — ADR-043 로드맵의 NIC
  클래스는 이 계획에 포함하지 않는다.
- **virtio-gpu 프레임버퍼**, 실기 표준 드라이버(AHCI/NVMe/e1000/
  VESA·GOP) — ADR-043의 2순위 항목은 전부 후속.
- **3D 가속/GPU 컴퓨트** — ADR-043이 이미 로드맵 밖으로 명시.
- **SWAPFS/메모리 압박 정책**(kernel-memory.md ADR-104~108/110),
  **우선순위 승격·정책 서버**(ADR-025/027, ADR-051) — 메모리/스케줄러
  쪽 후속 정책 과제로, 이 계획은 "다중 프로세스 시스템이 최소한
  동작한다"는 수직 슬라이스에 집중한다.
- **ext4 저널(JBD2) 재생/쓰기, htree 인덱스 활용, 64bit/metadata_csum/
  encrypt 등 확장 기능**(ADR-129가 v1 범위에서 명시적으로 제외) —
  필요성이 확인되면 별도 ADR.
- **USB Mass Storage·USB 네트워크 어댑터 등 HID 이외의 USB 장치
  클래스**(ADR-130이 명시적으로 제외) — 필요해지면 별도 ADR.
- **aarch64 이식** — 여전히 x86_64 우선(ADR-009), 별도 계획.
- **자동화 CI 파이프라인** — 이전 계획들과 동일하게 범위 밖.

## 검증 방법

kernel-bootstrap.md·smp-fpu-bringup.md와 같은 방식 — QEMU 부팅(M17부터는
콘솔 화면 상호작용 포함) 로그/화면으로 확인 가능한 마일스톤별 완료
기준을 두고, `tools/smoke-test-x86_64.sh`(또는 M17 이후 새 상호작용
스모크 스크립트)에 확인 문자열을 마일스톤마다 추가한다.

## 완료 후

각 마일스톤(또는 몇 개씩 묶어) 완료 시 `docs/done/`에 결과를 기록한다.
이 계획 문서 자체는 실행 후에도 수정하지 않고 "실행 전 계획" 그대로
보존한다.
