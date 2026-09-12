# minicore 아키텍처 (ARCHITECTURE)

이 문서는 minicore가 **현재 실제로 어떤 구조로 되어 있는지**를 요약한다.
"왜 이렇게 설계했는가"는 [CONCEPTS.md](CONCEPTS.md)를, 각 결정의 정확한
근거·대안·실행 중 발견한 버그는 `docs/design/*.md`의 ADR(2026-09-11
기준 230건, [docs/design/index.md](docs/design/index.md)가 주제별 안내),
프로토콜 상세는 `docs/spec/*.md`가 정본이다. 이 문서는 그 방대한 자료를
한눈에 훑기 위한 지도이지, 그것들을 대체하지 않는다.

## 0. 한눈에 보기

```
하드웨어 (x86_64, QEMU q35 / 실기 GRUB Multiboot2·UEFI)
  └─ 커널 (kernel/) — IPC·스케줄러·객체/핸들·페이지 할당자·코어 드라이버 3종만
       └─ initrun (init/initrun) — 최초 유저 프로세스, 유일한 최초 신뢰 루트
            └─ "커널 서버" 순차 기동(procsrv/vfs/fs/devmgr/drivers/cfgsrv/
                pipesrv/netsrv/login 등, 준비완료 신호 대기하며 순서대로)
                 └─ svcmgr (유저 서비스 관리자, systemd류) — initrun이 마지막
                     으로 spawn. initrun 소멸 후 프로세스 트리의 영구 루트가
                     되고, 남은 커널 서버들이 이 데몬 아래로 재부모화된다.
                          └─ login → msh(포팅된 셸, coreutils 내장) → musl
                              애플리케이션들
```

커널은 이 트리에서 맨 위 한 칸뿐이다 — 그 아래는 전부 유저 프로세스이며,
프로세스 사이의 유일한 통신 수단은 IPC다(CONCEPTS.md §3~4).

## 1. 커널 코어 (`kernel/`)

`kernel/core/`는 아키텍처 독립, `kernel/arch/<arch>/`는 아키텍처 의존
코드만 담는다(HAL 경계, ADR-002) — 코어는 arch 헤더를 직접 include하지
않는다.

### 1.1 IPC (`kernel/core/ipc/`)

- 두 원시: **Call/Reply**(동기 RPC, `sys_call`/`sys_recv`/`sys_reply`)와
  **Notification**(비동기 소실 없는 64비트 비트셋, `sys_notify`/
  `sys_wait`). 실패하지 않는다는 것이 Notification의 핵심 계약이다.
- `message` 구조: `label`(오퍼레이션 식별자) + `regs[4]`(레지스터,
  소량 데이터) + `pages[≤4]`(대용량 데이터, copy/move/map 세 모드) +
  `handles[≤2]`(핸들 위임).
- **cross-process 확장**(M13, ADR-151) — 서로 다른 유저 프로세스가
  직접 통신할 수 있다. 메시지 구조체 자체는 커널이 발신자/수신자
  페이지테이블로 가상주소를 번역해 옮긴다.
- **`pages[]`의 유저 프로세스 매핑**(ADR-159/161) — 수신자가 유저
  프로세스면 스레드별 고정 슬롯에 자동 매핑되고 다음 `sys_recv`
  직전에 자동 해제된다. 이 슬롯은 원래 **프로세스당 하나**였다가,
  M56에서 pthread 여러 개가 동시에 서로 다른 IPC 응답을 받는 시나리오가
  처음 나타나며 슬롯 충돌(한쪽이 다른 쪽 응답을 덮어씀)이 실제
  데이터 손상으로 드러났다 — 지금은 **스레드별 부분 슬롯**(64스레드
  ×16KiB, 슬롯4의 1MiB 예산 그대로 재분할)으로 해결되어 있다(ADR-229).
- **재진입(nested) IPC**(M42, ADR-216) — 서버 스레드가 자신이 받은
  호출에 아직 회신하지 않은 채 스스로 클라이언트가 되어 다른 Call을
  보내고 응답을 받을 수 있다. "가장 최근 recv"는 스레드당 재진입
  스택(깊이 4)으로 관리된다.
- **도네이션**(ADR-028) — `sys_call` 처리 시 대상 서버 스레드가 이미
  대기 중이면 호출자의 우선순위(boost_level)를 일시 상속시켜 즉시
  실행되게 한다. `sys_reply` 시 원래 값으로 복귀.
- **프록시 체인**(ADR-032) — 엔드포인트 프록시는 다시 프록시를 낳을
  수 있다. 순환은 커널이 생성 시점에 구조적으로 차단하며, 깊이는
  기본 64로 제한한다.

### 1.2 객체·핸들 모델 (`kernel/core/object/`)

- 모든 커널 객체(`thread`/`address_space`/`endpoint`/`notification`/
  `reg_table`)는 **프로세스별 핸들 테이블**의 인덱스로만 참조된다.
- 각 객체는 정확히 하나의 **소유 핸들**과 그 위에 트리로 위임되는
  0개 이상의 **프록시**를 가진다. 프록시의 rights는 항상 부모의
  부분집합이며, 소유자가 핸들을 닫으면 **cascade revoke**로 자손
  프록시 전체가 즉시 무효화된다.
- `address_space`는 두 개의 불변(생성 시 한 번만 설정, 이후 변경
  불가) 속성을 갖는다: `trusted`(true면 어떤 프로세스도 디버그
  접근 불가), `confinement_tier`(`normal`/`guest`/`jail`).
- **`handle_table` 동시성**(ADR-229) — M11부터 락이 없다는 것이
  지적돼 있었지만(OPEN-68), 실제 다중 코어 동시 IPC 시나리오가
  나타난 M56에 이르러서야 진짜 데이터 손상으로 확인됐다. 지금은
  `create_owner`/`create_proxy`/`close`/`handle_info`/`debug_entry`
  진입점 전체를 전역 스핀락 하나로 감싼다.

### 1.3 메모리 (`kernel/core/mm/`)

- **물리 페이지 할당자**: `per_cpu_cache`(코어 로컬, order-0 전용,
  락 없음) → `per_node_pool`(NUMA 노드당 1개, buddy, 스핀락 1개) 2단
  구조. 노드 간 폴백은 ACPI SLIT 기반 거리순(없으면 라운드로빈).
- **커널 힙**: 고정 크기 클래스(16~4096바이트) 슬랩.
- **COW(Copy-on-Write)**: 프레임 참조 카운트+PTE 소프트웨어 비트+쓰기
  폴트 분기로 `fork()`의 주소공간 복제를 구현한다(ADR-140~142).
- **유저 힙**: `sys_brk`(고정 1MiB 슬롯)와 `sys_mmap_anon`(별도 4MiB
  슬롯, `sys_brk`와 절대 공유하지 않는다 — musl 자신의 malloc이
  둘 다 시도할 수 있어 커서 오염을 막기 위해 분리) 두 경로가 공존한다.
- **가상주소 레이아웃**(x86_64, 4-level/48비트): 유저 공간
  `0x0` ~128TiB, physmap `0xFFFF800000000000`(512GiB, 물리 다이렉트맵),
  커널 스택 `0xFFFFFFFF00000000`(2GiB, 슬롯+가드 페이지), 커널 이미지
  `0xFFFFFFFF80000000`(≤2GiB, 섹션별 권한).
- **유저 프로세스 고정 가상주소 슬롯**(`k_user_stack_top` 기준
  오프셋 사다리) — 스택, argv, DMA 버퍼, MMIO 매핑(`sys_map_phys`,
  **프로세스당 슬롯 하나만 재사용** — 두 번째 매핑 호출은 첫 번째를
  덮어쓴다, ADR-007/038/039), IPC pages 매핑, 힙(brk), mmap_anon 순서로
  1MiB씩 배정되어 있다 — 서로 겹치지 않게 미리 예약된 표를
  `kernel-memory.md`가 관리한다.
- **쿼터**: 프로세스별 `limit_bytes`/`used_bytes`. 정책 서버가 아직
  없어 v1은 사실상 무제한.

### 1.4 스케줄러 (`kernel/core/sched/`)

- 두 우선순위 밴드: `kernel`(코어 드라이버 등)이 `user`보다 항상
  우선하며, `user` 밴드는 승격되어도 `kernel` 밴드를 절대 넘지 못한다.
- 실행 큐는 **NUMA 노드 단위**(코어 단위 아님). 코어는 자기 노드
  큐를 먼저 보고, 비었으면 다른 노드 큐를 훔친다(워크 스틸링,
  밴드 우선순위가 노드 경계보다 먼저 적용됨).
- **진짜 멀티코어 선점**(ADR-208/209) — 코어별 `g_current`, AP도
  유저 밴드에 참여하며 자기 LAPIC 타이머로 독립 선점한다. 타이머는
  HPET(1순위)/PIT(폴백) 실측으로 코어마다 보정된다. 이를 위해
  코어별 사설 GDT/TSS, SYSCALL 진입 시 코어별 커널 스택 포인터
  (swapgs+`IA32_KERNEL_GS_BASE`)가 필요했다 — 둘 다 원래 전역
  공유였다가 실제 멀티코어 트리플폴트를 겪고서야 분리됐다.
- **우선순위 승격**과 **도네이션**(§1.1)은 함께 동작한다 — user
  밴드 내부의 상대적 가중치일 뿐, kernel 밴드로의 전환은 아니다.

### 1.5 프로세스·스레드 원시

- `sys_fork`(handle_table 전체를 프록시로 복제), `sys_exec`(ET_DYN
  로더 지원까지는 있으나 동적 링킹 자체는 정적 링킹으로 되돌아감,
  ADR-203), `sys_process_spawn`, `sys_process_kill`, `sys_thread_create`
  (owner_space/handle_table을 fork처럼 클론하지 않고 **공유** —
  pthread용), `sys_futex`(WAIT/WAKE).
- **시그널**: `pending_signals`/`signal_mask`/`sigactions[32]`+
  `sys_signal_action`/`sys_signal_send`/`sys_rt_sigreturn`, syscall
  리턴 시점에만 전달 확인(IRETQ 경로·SIGCHLD 자동 전달·SIGKILL 즉시
  unlink는 범위 밖, OPEN-65). `SIGINT` 하나만 핸들러가 없으면 실제로
  프로세스를 종료시킨다(ADR-226) — 나머지 31개는 여전히 무시(OPEN-75).

## 2. 부팅 흐름

1. **진입점** — x86_64는 Multiboot2(1순위, GRUB 등 표준 부트로더)와
   UEFI(PE32+ 애플리케이션, 별도 `efi_main`) 둘 다 지원하며 커널 본체
   코드는 100% 공유하고 진입 스텁만 다르다. aarch64는 FDT 기반 2순위
   경로로 설계만 있고 아직 커널 코드 자체가 없다(ADR-009, x86_64
   완주 후 이식 예정). QEMU 개발 반복용으로 `qboot.rom`(PVH 스텁)
   직접 부팅 경로가 따로 있는데, **이것은 실제 배포 경로가 아니다** —
   BIOS/GRUB을 거치지 않으므로 VGA 텍스트 모드 세팅 같은 BIOS가
   해 주던 일을 커널이 직접 해야 한다(§5.1 참고).
2. 각 진입 경로가 공통 `boot_info` 구조체(메모리맵, NUMA 토폴로지,
   initrd 위치, cmdline, ACPI RSDP 등)로 수렴한다 — 커널 코어는 이
   구조체만 알고 Multiboot2/UEFI/FDT의 존재 자체를 모른다.
3. **initrd**는 자체 포맷 MCPACK v1(서드파티 tar/cpio 파서 없음)로
   `initrun` 하나만 담는다. 커널이 이를 ELF로 로드해 새 주소공간을
   만들고 **무조건 `trusted=true`**로 생성한다 — initrun은 시스템의
   유일한 최초 신뢰 루트다.
4. **initrun**(`init/initrun`)이 `boot_device_descriptor`로 부트
   디바이스(virtio-blk)를 직접 마운트하는 임베디드 최소 클라이언트와
   cpio(newc) 리더를 갖고 있다 — `tools/mkbootdisk.py`가 만든 부트
   디스크에서 "커널 서버"(procsrv/vfs/fs/devmgr/drivers/cfgsrv/
   pipesrv/netsrv/login 등, 하드코딩된 이름·순서)를 찾아 각자의
   준비완료 신호(spawn 시점에 만들어지는 전용 endpoint의 Call/Reply)
   를 기다리며 순서대로 기동한다.
5. 커널 서버를 전부 띄운 뒤, initrun은 **마지막으로** 유저 서비스
   관리자 `servers/svcmgr`를 spawn하고 스스로 사라진다. 이 시점에
   procsrv가 initrun의 남은 자식들(커널 서버들)을 svcmgr 아래로
   **재부모화**한다 — 프로세스 트리의 영구 루트는 svcmgr다.

## 3. 서버 토폴로지 (`servers/`)

| 서버 | 역할 |
|---|---|
| `procsrv` | 프로세스 테이블(pid/parent_pid/state/exit_code), fork/exec/wait/kill, 계정·로그인·su/sudo 신원 관리, `/sys/proc` synthetic FS 겸임 |
| `vfs` | 경로 탐색·마운트 테이블·FS 서버로 라우팅 |
| `fs/memfs` | 메모리 파일시스템, 읽기/쓰기 모두 지원(커서 기반), `OP_LIST` |
| `fs/fat32`, `fs/ext4` | 읽기전용 |
| `devmgr` | ACPI/PCIe(ECAM) 버스 열거, 드라이버 등록·매칭, 핫플러그 |
| `drivers/console` | VGA 텍스트 콘솔 — 실제 하드웨어 레지스터 초기화 포함(§5.1) |
| `drivers/ps2` | 키보드 입력 |
| `drivers/virtio-blk`, `drivers/virtio-net` | 블록/네트워크 유저 드라이버 |
| `drivers/usb` | xHCI 리셋+포트 스캔까지(실제 장치 열거는 범위 밖) |
| `cfgsrv` | 설정 리포지터리(§4) |
| `pipesrv` | 익명 파이프(`pipe()`/`dup2()`) — 프로토콜-레벨 정수 id + 참조 카운트 |
| `netsrv` | 이더넷/IP/UDP 프레이밍, DHCP 왕복 |
| `svcmgr` | 유저 서비스 관리자(systemd류), 프로세스 트리 영구 루트(§2) |
| `login` | 콘솔마다 하나, 인증 후 세션 프로세스(현재 `msh`) 실행 |

모든 커널 서버는 `k`(libk)+`mc`(libmc)만 링크한다 — libc가 필요 없다
(CONCEPTS.md §6).

## 4. VFS 레이아웃과 레지스트리

### 4.1 런타임 파일시스템 (`docs/spec/vfs-layout.md`)

```
/boot                  부팅 루트 (Multiboot2 경로: 일반 파일 / UEFI 경로: /boot/uefi에 FAT32 ESP)
/sys/proc              procsrv가 FS 서버 겸임
/sys/dev               devmgr가 FS 서버 겸임
/sys/live/*            여러 서버가 하위 경로별로 분담 (network→netsrv, sched→procsrv 등)
/sys/etc, /sys/bin      전역 시스템 설정/바이너리
/sys/tmp               memfs 재마운트
/run                   공용 설치 소프트웨어 (guest/jail의 실행 허용 범위, ADR-082)
/usr/{사용자명}/{bin,lib,etc,home}   사용자별 격리 루트
/home/{사용자명}        /usr/{사용자명}/home으로의 순수 경로 재작성(별칭, 실체 없음)
```

표준 FHS와 의도적으로 다르다(`/usr`/`/run`의 의미가 바뀜) — 포팅
소프트웨어의 하드코딩된 경로는 심볼릭 링크 호환 계층 대신 `libc`
sysdeps 상수 + 개별 패치로 대응한다.

### 4.2 설정 리포지터리 — cfgsrv (`docs/spec/registry.md`)

Windows 레지스트리에서 착안한 **VFS와 완전히 분리된** 전용 IPC
프로토콜(`reg_op`: open_table/create_table/delete_table/list_children/
get_value/set_value/delete_value/list_values/set_permissions). 주소는
`@스키마/A/B/table` 형태이며 스키마 생략 시 호출자 계정 이름이 강제
적용된다. `global` 스키마는 예약어(root 전용). 값은 string/int64/
boolean/binary 중 하나이며, Unix 스타일 owner/group/other RWX 권한이
중간 경로와 테이블 각각에 붙는다. 실제 저장은 `/sys/etc/registry.dat`
(memfs)에 전체 상태를 매 쓰기마다 재직렬화하는 방식이다.

`@global/system/services`가 svcmgr의 유닛 레지스트리다 — 새 값 타입
없이 기존 binary 값 하나에 `service_unit` 구조체를 그대로 담는다.

## 5. 드라이버 계층

### 5.1 콘솔 — 실제 VGA 하드웨어 초기화 (ADR-230)

`qboot.rom` 개발 경로는 BIOS를 거치지 않으므로, VGA 카드를 "80x25
텍스트 모드"로 세팅해 주는 존재가 원래 없다. `servers/drivers/console`
이 실제 VGA BIOS의 INT 10h AH=00h AL=03h와 같은 레지스터 시퀀스를
직접 재현한다:

1. Miscellaneous Output → Sequencer → CRTC → Graphics Controller →
   Attribute Controller 순서로 텍스트 모드 3 레지스터를 프로그래밍
   (`sys_io_activate`로 포트 0x3B0~0x3DF 권한을 얻는다, ps2 드라이버와
   같은 패턴).
2. DAC(0x3C8/0x3C9)에 EGA/VGA 표준 16색 팔레트를 직접 로드한다 —
   Attribute Controller는 속성 니블→DAC 인덱스 매핑만 할 뿐 실제
   RGB 값은 정의하지 않는다.
3. VRAM "플레인 2"(문자 생성기 플레인)에 글리프 비트맵을 직접
   쓴다 — Sequencer를 Synchronous Reset으로 잠깐 멈추고 Map Mask/
   Memory Mode/Graphics Controller를 재구성해 물리주소 0xA0000을
   플레인 2에 선형 매핑한 뒤 쓰고 복원한다. `servers/login`이 실제로
   출력하는 ~20글자만 손으로 그린 8x8 비트맵으로 담는다(전체 ASCII
   폰트는 OPEN-77로 미룸).
4. `sys_map_phys`가 프로세스당 고정 가상주소 슬롯 하나만 재사용한다는
   것(§1.3) 때문에, 폰트 로드용 임시 매핑을 **먼저** 끝내고 텍스트
   버퍼(0xB8000) 매핑을 **마지막에** 해야 한다 — 순서를 반대로 하면
   두 번째 매핑이 첫 번째를 조용히 덮어써 화면이 계속 검게 남는다.

### 5.2 devmgr — 버스 열거와 드라이버 매칭

MCFG(ACPI)로 ECAM 영역을 찾아 PCIe를 재귀 열거하고, 드라이버가
`register_driver`로 알린 vendor/device ID(또는 class code)와 매칭해
BAR/IRQ 정보를 넘긴다. 커널에는 PCIe 전용 개념이 없다 — 전부 기존
MMIO 캐패빌리티(`sys_map_phys`)와 notification 재사용이다.

## 6. 보안·신원 모델

- **신원**: `identity_badge`(uid/gid + super/guest/jail 3비트 +
  jail_instance_id). procsrv가 `@global/system/users`/`groups`에
  영속화하며 발급 주체다.
- **신원 변경은 항상 새 프로세스 생성**(불변 원칙, CONCEPTS.md §8) —
  로그인 성공 시 procsrv가 새 `address_space`/`primary_thread`를
  만들며 이 시점에 딱 한 번 `trusted`/`confinement_tier`를 정한다.
- **격리 등급**: `normal`/`guest`/`jail`. guest/jail은 VFS 접근이
  자기 홈(`/home/`) 밖으로 못 나가고(`GUEST_DENIED`), 실행도 `/run`
  서브트리 안으로 제한된다(ADR-082). jail은 추가로 에페메럴 오버레이
  네임스페이스를 배정받는다.
- **위임(su/sudo)**: 대상 계정이 미리 등록해 둔
  `@global/system/delegates/<계정>` 테이블을 조회하는 방식 — 전역
  sudoers 목록이 아니라 탈중앙화된 위임이다. 기간 모드(기본값/명시적/
  영구)와 명령 단위 제한(`allowed_command_paths`)까지 지원한다.
- **badge 기반 강제**는 아직 전체 시스템에 걸쳐 있지 않다 — 대부분의
  프로토콜(VFS의 guest/jail 신원, cfgsrv의 uid, procsrv의 caller_pid)
  은 여전히 호출자의 자기 선언이다. badge를 실제로 위조 불가능하게
  검증하는 경로는 지금까지 svcmgr↔procsrv 한 조합에만 연결되어
  있다(ADR-217, `docs/design/open-items.md`의 OPEN-38/60/67/72가
  나머지 갭을 추적한다).

## 7. 유저랜드 스택

### 7.1 mc/k 계층 (`libs/`)

CONCEPTS.md §6의 3단 구조 그대로 — `libs/k`(C++ freestanding 유틸),
`libs/mc`(순수 C, syscall 1:1 래퍼+서버별 프로토콜 클라이언트,
`mc/syscall.h`가 커널·유저 공용 syscall ABI의 유일한 정본).

### 7.2 musl libc

정적 링킹만 지원한다(ADR-203 — musl 자신의 공유 `libc.so` 동적
링커 자기재배치 부트스트랩은 시도하지 않았다). 커널을 확장하는
대신 musl의 `syscall_arch.h`를 패치해 모든 syscall을
`libc/sysdeps/minicore/syscall_shim.c`로 우회시킨다:

| shim 파일 | 대체하는 것 |
|---|---|
| `syscall_shim.c` | 모든 Linux syscall 번호 → mc/커널 syscall 변환 |
| `malloc_shim.c` | musl 자신의 `SYS_mmap` 기반 malloc(구 `mem_shim.c`는 M30에서 대체됨) |
| `clone_shim.c` | musl `__clone`(raw Linux ABI 어셈블러)의 순수 C 대체 |
| `lock_shim.c` | `__lock`/`__unlock`을 진짜 futex로 |
| `locale_shim.c` | `__map_file`(로케일 탐색) 대체, C/POSIX 고정 |
| `set_thread_area.c` | TLS(FS_BASE) 설정 |

pthread(`sys_thread_create`+`sys_futex`), 표준 signal, `setlocale()`
(C/POSIX 고정), stdio(fopen/fread/printf 등 musl 소스 그대로)까지
동작한다.

### 7.3 msh — 포팅 대신 자체 작성한 셸

BusyBox 도입을 철회한 뒤(ADR-221, CONCEPTS.md §2) 직접 작성한 최소
셸이다. 로그인 성공 시 procsrv가 직접 `sys_process_spawn`으로
띄운다(구 `userland/shell`은 완전히 제거됨, ADR-224).

- `|`(파이프라인)/`>`(출력 리다이렉션) — execve()가 지우는 fd
  테이블을 우회하기 위해 `mc/shell_fd_binding.h`가 파이프/리다이렉션
  대상을 argv의 `"@pipefd"`/`"@filefd"` 토큰으로 실어 보낸다.
- **coreutils가 별도 ELF가 아니라 msh 자신의 빌트인이다**
  (`echo`/`ls`/`cat`/`[`, ADR-228) — 파이프라인의 여러 단계가
  동시에 진행돼야 하므로 각 빌트인을 진짜 musl pthread로 병렬
  실행한다. pthread는 handle_table/owner_space를 공유하므로, 빌트인은
  M54의 "fd 번호" 계층을 완전히 우회해 raw `pipe_id`/
  `{fs_handle, open_file_id}`를 함수 인자로 직접 받는다. 알려지지
  않은 명령은 여전히 기존 fork+exec 경로로 떨어진다.
- **job control 최소**(ADR-226/227) — msh가 자기 자식의 핸들로
  직접 `SIGINT`를 보내고, 새 procsrv 오퍼레이션
  `MC_PROC_OP_REPORT_SIGNALED`로 결과를 통지한다. 실제 PS/2 키보드의
  Ctrl-C 스캔코드에서 이 경로를 트리거하는 연결은 아직 없다(OPEN-76).

## 8. 저장소 구조 (요약)

```
kernel/            커널 (core=arch 독립, arch/<arch>=아키텍처 의존)
libs/k/            libk — 커널·서버 공용 C++ freestanding 유틸
libs/mc/           libmc — syscall ABI 정본 + C 바인딩 (커널·유저 공용)
init/initrun/      최초 유저 프로세스
servers/           procsrv/vfs/fs/devmgr/drivers/cfgsrv/pipesrv/netsrv/svcmgr/login
libc/              포팅된 musl + sysdeps/minicore 어댑터
userland/          msh 등 minicore 전용 유저 프로그램
third_party/       git submodule 원본(무수정) + patches/
tools/             mkbootdisk.py, run-qemu.sh, smoke-test-*.sh 등
docs/              spec/plan/done/design/remind (문서 체계는 CONCEPTS.md §9)
```

자세한 CMake 오케스트레이션·서드파티 패치 흐름은
[docs/design/repo-layout.md](docs/design/repo-layout.md) 참고.

## 9. 현재 상태

x86_64에서 부팅→IPC→멀티코어 스케줄링→VFS/파일시스템→디바이스
드라이버(virtio-blk/virtio-net/ps2/console/USB)→설정 리포지터리→
계정/로그인/su-sudo→유저 서비스 관리자→포팅된 musl libc(정적 링킹,
pthread/signal/locale 포함)→자체 작성 셸(msh, 파이프라인+coreutils
빌트인+job control 최소)까지 전부 QEMU에서 동작하며, 5개 회귀
스위트(스모크/SMP/NUMA/AVX/net)로 매 변경마다 검증한다. 실제 VGA
텍스트 모드로 QEMU 창에 로그인 프롬프트가 육안으로 보인다(ADR-230).
aarch64는 아직 커널 코드 자체가 없다(설계만 있음, ADR-009). 완료된
계획들의 전체 이력은 `CLAUDE.md`, 완료 보고 전체 목록은
[docs/index.md](docs/index.md#done--완료-보고)를 참고한다.
