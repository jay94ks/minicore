# 실행 계획: 범용 운영체제 완성 (M21~M26) — 선점형 스케줄링부터 최소 네트워킹까지

**관련 결정**: [kernel-scheduler.md](../design/kernel-scheduler.md)
(ADR-014·025·027·034·035·109·124), [foundations.md](../design/foundations.md)
(ADR-005·008·049·132), [security-model.md](../design/security-model.md)
(ADR-079·087~090), [kernel-memory.md](../design/kernel-memory.md),
[boot-and-drivers.md](../design/boot-and-drivers.md)(ADR-043 로드맵의
netsrv/virtio-net 자리)

**선행 완료 전제**: [kernel-bootstrap.md](kernel-bootstrap.md)(M1~M8),
[smp-fpu-bringup.md](smp-fpu-bringup.md)(M9~M11b),
[system-servers-bringup.md](system-servers-bringup.md)(M12~M20) 전부
완료돼 있어야 한다. 실제 부팅 경로 검증
([real-hardware-boot-verification.md](../done/real-hardware-boot-verification.md))
도 이미 끝나 있다.

## 배경

M12~M20이 증명한 것은 "procsrv/vfs/cfgsrv/셸이 한 번의 부팅
시나리오에서 서로 IPC로 통신하는 수직 슬라이스"다 — 실제 다중
프로세스가 오래 실행되는 범용 운영체제라면 당연히 가져야 할
성질들은 그동안 계획적으로 미뤄 왔다(각 M12~M20 완료 보고서의
"알려진 단순화" 절, ADR-124의 "영향" 절 등에 이미 명시적으로 기록돼
있다). 다섯 가지 공통분모로 정리된다:

- **선점형 멀티태스킹이 없다** — [kernel-scheduler.md](../design/kernel-scheduler.md)
  ADR-124가 이름 그대로 지적한 "선점 없는 협조적 스케줄러"가 M1부터
  지금까지 그대로다. 타이머 인터럽트가 스레드를 강제로 내리는
  경로 자체가 없다 — 무한루프 도는 유저 프로세스 하나가 코어 전체를
  영구히 점유할 수 있다.
- **프로세스 생명주기가 미완성이다** — procsrv에 진짜 프로세스
  테이블이 없고, 부모가 자식의 종료를 기다리는 `wait()`류도, 시그널
  (`SIGKILL` 등 최소한의 강제 종료 신호)도 없다. ADR-124 자신이
  "자원 회수·부모 알림(wait/exit status)은 그 시점에 별도로
  설계해야 한다"고 이미 예고해 뒀다.
- **fork/exec의 fd 상속이 실제로 동작하지 않는다** — ADR-008
  (foundations.md)이 이 프로젝트에서 "가장 어려운 문제"로 지적한
  그대로다. M20은 셸의 `ls`/`cat`을 빌트인으로 만들어 이 문제를
  정면으로 피해 갔다([system-servers-bringup-m20.md](../done/system-servers-bringup-m20.md)).
- **유저랜드에 동적 메모리가 없다** — `mmap`/`brk` 유사 syscall이
  없어 모든 서버가 정적 버퍼만 쓴다. 실제 libc(malloc)를 포팅하려면
  이게 먼저 있어야 한다.
- **네트워킹이 전혀 없다** — `servers/netsrv`는 여전히 빈 `INTERFACE`
  라이브러리다(ADR-006/008이 자리만 잡아 둔 것, 실제 소켓/프로토콜
  스택은 아무것도 없다).

이 다섯은 M12~M20 각각이 "지금은 범위 밖"으로 미뤄 온 것들 중,
"그 마일스톤만의 특수한 사정"이 아니라 **"범용 운영체제가 되려면
결국 다 있어야 하는 것"**이라는 공통점이 있어 이 계획으로 한데
모은다. aarch64 이식(ADR-009)은 이 계획이 다루는 어떤 것에도
기술적으로 의존하지 않지만, 사용자가 "다른 아키텍처를 시도하기
전에" 이 계획을 먼저 하기로 명시적으로 결정했다 — x86_64 위에서
검증하는 쪽이 새 아키텍처 이식 중 발생하는 문제와 이 계획이 다루는
문제를 뒤섞지 않고 구분하기 쉽다.

**이 계획에 포함하지 않는 것**(이미 설계는 끝났지만 이 계획의
"범용 OS 최소 성립 조건"과는 결이 다른 것들, 또는 개별 서브시스템의
심화 작업):

- SWAPFS/메모리 압박 정책([kernel-memory.md](../design/kernel-memory.md)
  ADR-104~108/110), 우선순위 승격·정책 서버(ADR-025/027/051) —
  이미 설계는 끝나 있고 "언제 구현할까"만 남았다. 성능/QoS 다듬기에
  가까워 이 계획의 다섯 가지 공통분모(프로세스가 범용 OS답게
  스케줄되고, 살고, 죽고, 통신하는가)와는 우선순위가 다르다.
- FAT32/ext4 쓰기 지원, jail 실제 오버레이 네임스페이스(ADR-080/081),
  cfgsrv의 group 권한·진짜 커널 `reg_table` 객체 — 각각 M16/M18/M19가
  이미 "알려진 단순화"로 명시해 둔 항목이고, 개별 서브시스템 심화라
  이 계획의 범위와 다르다.
- **실제 서드파티 libc/셸 포팅** — 이 계획(M21~M24)이 만드는
  선점/wait·시그널/fork·exec/malloc은 정확히 그 포팅의 전제
  조건이다. 이 계획의 마지막 마일스톤(M26)이 다시 시도하되,
  구체적 범위는 그 시점에 다시 좁힌다(M17~M20이 반복해 온 "세부는
  착수 시점에 확정" 패턴 그대로).
- USB Mass Storage 등 HID 이외 USB 장치 클래스, 실기 표준 드라이버
  (AHCI/NVMe/e1000/VESA/GOP), GPU/3D, 자동화 CI 파이프라인 —
  이전 계획들이 이미 범위 밖으로 명시해 둔 항목 그대로 유지한다.
- aarch64 이식 — 사용자가 이 계획 완료 후로 명시적으로 미룸.

## M21. 선점형 스케줄링

- **구현**: LAPIC 타이머(`kernel/arch/x86_64/lapic.cpp` — 지금은
  초기화만 하고 주기 인터럽트를 걸지 않는다)를 주기 모드로
  재프로그램하고, 새 IDT 벡터로 라우팅해 스케줄러의 타임슬라이스
  소진 시 강제로 다음 스레드로 전환한다(현재 스레드가 `yield()`를
  스스로 부르지 않아도 넘어가야 한다). [scheduler.md](../spec/scheduler.md)
  §2가 이미 정의해 둔 `base_time_slice_us`/`boost_level` 필드를
  실제로 소비한다.
- **목표**: 무한루프만 도는 유저 스레드 하나와, 그 옆에서 자기
  카운터를 증가시키는 다른 스레드를 동시에 띄워, 타이머 선점 없이는
  절대 진행할 수 없는 두 번째 스레드가 실제로 계속 진행됨을 QEMU
  로그로 증명한다(SMP 여러 코어에서도 코어별로 독립적으로 동작함을
  `MINICORE_QEMU_SMP`로 함께 확인).

## M22. 프로세스 생명주기 — procsrv 프로세스 테이블 + wait/exit status + 최소 시그널

- **구현**: [procsrv.md](../spec/procsrv.md)(OPEN-54가 아직 정하지
  않은 실제 와이어 프로토콜을 이 마일스톤이 최소 범위로 확정한다)의
  프로세스 테이블(pid/parent/상태/exit code)을 실제로 만든다.
  procsrv IPC에 `OP_WAIT`(자식 종료 대기, exit code 회수)와
  `OP_KILL`(최소 시그널 — 이번 라운드는 `SIGKILL` 수준의 "즉시 강제
  종료"만, 핸들러 등록·`SIGCHLD` 같은 세밀한 시그널 의미론은 범위
  밖)을 추가한다.
- **목표**: procsrv가 자식 프로세스를 하나 spawn한 뒤 종료시키고,
  `OP_WAIT`로 그 종료를 회수해 exit code가 일치하는지 확인한다.
  별도 프로세스에 `OP_KILL`을 보내 강제 종료됨을 확인한다.

## M23. 진짜 fork/exec — procsrv가 fd 진실 공급원 역할을 실제로 수행

- **구현**: ADR-008(foundations.md)이 "가장 어려운 문제"로 지적한
  그대로 — procsrv가 프로세스별 fd 테이블((서버 캐패빌리티, 서버측
  핸들) 쌍)을 실제로 관리하고, `sys_fork` 시 부모의 fd 테이블을
  자식에게 그대로 상속시킨다(ADR-023의 프록시 위임 메커니즘 재사용).
  M20의 셸이 지금은 빌트인으로만 처리하는 `ls`/`cat`을 이 마일스톤
  부터는 **진짜 별도 프로세스**(procsrv가 fork+exec으로 스폰)로
  바꿀 수 있는지 검증한다.
- **목표**: 셸이 VFS에 파일을 하나 열어 둔 채로 fork해, 자식이
  **상속받은 그 fd로** 별도의 exec 이미지에서 읽기를 계속할 수
  있음을 증명한다.

## M24. 유저랜드 동적 메모리

- **구현**: 새 syscall(`sys_mmap` 또는 `sys_brk`류 — 이름과 정확한
  시맨틱은 착수 시점에 확정, 익명 페이지를 프로세스 주소공간에
  늘리는 최소 기능이면 충분하다) + `libmc`에 최소 `malloc`/`free`
  (단순 free-list 또는 bump allocator) 구현.
- **목표**: `userland/shell`이 `libmc`의 malloc으로 힙 버퍼를
  할당해 쓰는 경로를 하나 추가해 왕복 확인한다.

## M25. 최소 네트워킹

- **구현**: `servers/netsrv`(지금은 빈 `INTERFACE` 라이브러리)를
  실제로 채운다 — `servers/drivers/virtio-net` 최소 드라이버 +
  netsrv의 최소 소켓 API(UDP 정도로 시작, TCP는 후속). devmgr가
  이미 하는 PCIe 열거로 virtio-net을 찾는다(M14의 패턴 재사용).
- **목표**: QEMU의 usermode 네트워킹(또는 loopback)으로 UDP 패킷
  하나를 왕복시켜 확인한다.

## M26. 실제 libc 포팅 재도전

- **구현**: M20(ADR-170)이 미뤄 둔 실제 서드파티 libc(musl 등)
  포팅을 M21~M24가 갖춘 전제(선점/wait·시그널/fork·exec/malloc)
  위에서 다시 시도한다. 구체적으로 어떤 libc를 채택할지, 어디까지
  포팅할지는 착수 시점에 다시 범위를 좁힌다(M17~M20이 반복해 온
  패턴 그대로).
- **목표**: M20의 최종 완료 기준("로그인 프롬프트를 통과하면 셸이
  뜨고 `ls`/`cat` 같은 기본 명령을 쓸 수 있다")을 이번엔 **포팅된
  libc + 포팅된 셸/coreutils**로 다시 달성한다.

## 범위 밖 (이 계획 이후로 미룸)

- SWAPFS/메모리 압박 정책, 우선순위 승격·정책 서버 — ADR-025/027/051/
  104~108/110이 이미 설계, 구현 시점 미정.
- FAT32/ext4 쓰기, jail 실제 오버레이(ADR-080/081), cfgsrv group
  권한·진짜 kernel `reg_table` 객체.
- USB Mass Storage 등 HID 이외 USB 장치 클래스, 실기 표준 드라이버
  (AHCI/NVMe/e1000/VESA/GOP), GPU/3D.
- aarch64 이식 — 사용자가 이 계획 완료 후로 명시적으로 미룸.
- 자동화 CI 파이프라인 — 이전 계획들과 동일하게 범위 밖.

## 검증 방법

kernel-bootstrap.md·smp-fpu-bringup.md·system-servers-bringup.md와
같은 방식 — QEMU 부팅 로그로 확인 가능한 마일스톤별 완료 기준을
두고, `tools/smoke-test-x86_64.sh`에 확인 문자열을 마일스톤마다
추가한다. M21(선점)은 SMP/NUMA 회귀 스위트에도 새 위험을 들여오므로
(타이머 인터럽트가 기존 IPI/TLB shootdown 경로와 상호작용할 수 있다)
착수 시 특히 꼼꼼히 재확인한다.

## 완료 후

각 마일스톤(또는 몇 개씩 묶어) 완료 시 `docs/done/`에 결과를
기록한다. 이 계획 문서 자체는 실행 후에도 수정하지 않고 "실행 전
계획" 그대로 보존한다.
