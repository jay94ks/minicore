# 실행 계획: 시스템 서버 로드맵 (M12~M20) — procsrv부터 로그인 후 셸까지

**관련 결정**: [kernel-ipc-objects.md](../design/kernel-ipc-objects.md),
[filesystem.md](../design/filesystem.md), [boot-and-drivers.md](../design/boot-and-drivers.md)
(ADR-038~043·056·057), [security-model.md](../design/security-model.md),
[registry-decisions.md](../design/registry-decisions.md)(ADR-060~064)

**선행 완료 전제**: [kernel-bootstrap.md](kernel-bootstrap.md)(M1~M8)과
[smp-fpu-bringup.md](smp-fpu-bringup.md)(M9~M11)이 끝나 있어야 한다 —
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

예외적으로 딱 하나, **OPEN-29(initrun 기동 매니페스트 형식)**는 아직
미결정이라 M12 착수 전에 새 ADR로 해소해야 한다 — 그 외에는 새로
결정할 것이 거의 없다.

## M12. procsrv 골격 — 프로세스 테이블·fork/exec·계정 모델·fd 진실 공급원

- **선행 결정 필요**: **OPEN-29**(initrun 기동 매니페스트 형식) — initrun이
  procsrv를 어떤 인자로, 어떤 순서로 실행할지 정하는 새 ADR을 착수 전에
  작성한다([boot-and-drivers.md](../design/boot-and-drivers.md) 대상).
- **구현**: [procsrv.md](../spec/procsrv.md) §프로세스 테이블·fork/exec
  시퀀스(ADR-016 COW 활용)·fd 진실 공급원 프로토콜, 계정
  모델([security-model.md](../design/security-model.md) ADR-079 —
  uid/gid 발급, ROOT/Supervisor 식별) 저장소까지. 로그인/session_program
  프로토콜 자체(§procsrv.md)는 이 마일스톤에서 **프로토콜 골격만** 만들고
  실제 콘솔 연동은 M17로 미룬다(procsrv.md 자신이 이미 이렇게 scope함).
- **목표(QEMU 검증)**: initrun이 새 ADR의 매니페스트로 procsrv를 실행하고,
  procsrv가 **자기 자신을 fork/exec**해 실제 두 번째 완전한 유저
  프로세스를 만들어내는 것을 로그로 확인한다 — M4~M8까지는 커널이
  직접 만든 스레드/프로세스뿐이었다.

## M13. VFS + memfs — fd 라우팅 실동작 + 최초 파일시스템

- **구현**: [filesystem.md](../design/filesystem.md) ADR-018(fd→서버
  캐패빌리티/핸들 매핑)을 M12의 fd 프로토콜과 실제로 연결하고,
  마운트 네임스페이스 기초(ADR-044/045)를 갖춘 `vfs` 서버를 만든다.
  최초 파일시스템은 순수 인메모리인 `fs/memfs`(repo-layout.md가
  "최초 구현 대상"으로 이미 지정) — 블록 드라이버 없이 VFS 프로토콜
  자체를 검증하는 게 목적이다.
- **목표**: M12의 두 번째 프로세스가 VFS 경유로 memfs에 파일을 쓰고
  다시 읽어 내용이 일치함을 확인한다.

## M14. devmgr + PCIe 버스 열거

- **구현**: [pcie.md](../spec/pcie.md), boot-and-drivers.md
  ADR-038~041(ECAM 설정공간 접근, devmgr 버스 열거, 핫플러그, 동적
  드라이버 등록). 아직 실제 드라이버는 만들지 않는다 — 열거만.
- **목표**: QEMU가 붙인 `virtio-blk`/`virtio-net`/`virtio-gpu` 각
  장치의 벤더/클래스 ID를 devmgr가 읽어 로그로 남긴다.

## M15. virtio-blk 드라이버 — 첫 실제 유저 드라이버

- **구현**: boot-and-drivers.md ADR-043의 1순위(스토리지 클래스).
  `servers/drivers/`에 virtio-blk 드라이버 신설, M14의 devmgr가
  이 드라이버를 자동 기동.
- **목표**: 알려진 패턴을 QEMU가 붙인 디스크 이미지에 블록 단위로
  쓰고 다시 읽어 일치함을 확인한다.

## M16. FAT32 FS 서버 (ADR-057)

- **구현**: ADR-057(FAT32를 virtio-blk 검증 직후 착수하기로 이미
  확정). `fs/fat32` 서버를 M15의 블록 드라이버 위에 올리고 M13의
  VFS에 마운트 지점으로 연결한다.
- **목표**: 호스트에서 미리 만든 FAT32 이미지의 파일을, 마운트 후
  VFS 경유로 열어 내용을 읽어낸다.

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
  `ls`/`cat` 같은 기본 명령으로 M13(memfs)·M16(FAT32) 위의 파일을
  조회할 수 있다 — **이 계획 전체의 최종 완료 기준**.

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
