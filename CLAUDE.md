# CLAUDE.md

minicore — **AI 네이티브 마이크로커널**. 이 저장소에서 작업할 때
지켜야 할 규칙이다.

## 문서 체계 (필수 준수)

전체 규칙은 [docs/remind/doc-convention.md](docs/remind/doc-convention.md)에
있다. 요약:

| 디렉토리 | 용도 | 제약 |
|---|---|---|
| `docs/spec/` | 명세 (무엇을 만들 것인가, 구현 가능한 수준) | 구현 방법이 아닌 요구사항/인터페이스 계약 |
| `docs/plan/` | 실행 계획 | **실행 전의 계획만** 남긴다 |
| `docs/done/` | 결과 보고 | **실행 완료된 것만** 기록한다 |
| `docs/design/` | 설계/상세 (ADR 등) | 구조·알고리즘·자료구조·트레이드오프 |
| `docs/remind/` | 기억 지시 사항 | 사용자의 "기억해" 지시만 |

**`docs/` 안의 모든 문서는 [docs/index.md](docs/index.md)에 리스팅되어야
한다** — 새 문서를 추가하거나 이름을 바꾸면 반드시 함께 갱신한다.

## 설계 결정 기록 규칙

- 모든 아키텍처 결정은 **주제별로 분리된** `docs/design/*.md` 파일에
  `ADR-NNN` 형식으로 기록한다. 전체 목록과 "이 주제는 어느 파일"
  안내는 [docs/design/index.md](docs/design/index.md)를 먼저 본다.
  번호는 파일이 나뉘어도 프로젝트 전체에서 계속 순차 증가한다 —
  가장 최근 ADR 번호(색인에서 확인)+1을 쓰고, 주제가 맞는 파일에
  추가한다. 맞는 파일이 없으면 새 파일을 만들고 색인에 등록한다.
  기존 ADR은 절대 수정하지 않는다 — 번복 시 새 ADR을 추가하고 옛
  ADR에 `상태: 대체됨 (→ ADR-NNN)`을 표시한다.
- 미결정 항목은 [docs/design/open-items.md](docs/design/open-items.md)
  한 곳에 모아 관리한다. 새 미결정 사항은 `OPEN-NN`으로 번호를 매겨
  "현재 열려있는 항목" 표에 추가하고, 해결되면 "해결된 항목" 표로
  옮기며(취소선 유지) 해결한 ADR 번호를 적는다.
- **기존 spec 문서가 새 ADR로 인해 낡은 내용을 갖게 되면 그 spec도
  함께 갱신한다** — "해결됨"이라고만 결정 기록에 적고 실제 스펙
  본문은 옛 상태로 방치하지 않는다.

## 답변 파일 (docs/reply.md)

`tools/docs-dashboard` 웹 대시보드의 "답변 입력" 탭에서 사용자가 입력한
내용이 `docs/reply.md`에 쌓인다. 항목은 관련 `OPEN-N` 추적 코드와 함께
기록되거나(미결정 항목에 대한 답변), 추적 코드 없이 일반 메모로 기록된다.

- `docs/reply.md`는 **미처리 답변함**이다 — 영구 로그가 아니다.
- 이 파일의 항목을 실제로 반영했다면(ADR 작성, spec/design 문서 갱신,
  `open-items.md`의 해당 `OPEN-N` 해결 처리 등) **그 즉시 해당 항목을
  `docs/reply.md`에서 삭제한다.** 처리 완료 후에도 남겨두지 않는다.
- 파일이 비면(모든 항목 삭제) 헤더만 남기거나 파일 자체를 지워도 된다.

## 코딩 컨벤션

[docs/spec/cxx-conventions.md](docs/spec/cxx-conventions.md) 참고. 핵심:

- 커널·시스템 서버(`kernel/`, `servers/*`, `libk/`)는 **snake_case**만
  쓴다. 순수 인터페이스는 `_interface` 포스트픽스(`I` 프리픽스 아님).
- 예외·RTTI 금지, freestanding 표준 헤더만 허용(ADR-003/010).
- 포팅된 코드(`libc/`, `userland/`)는 원본 프로젝트의 컨벤션을 따르며
  이 규칙의 적용 대상이 아니다.

## 저장소 구조 vs VFS 레이아웃 — 혼동 금지

두 문서는 이름이 비슷하지만 완전히 다른 대상을 다룬다:

- [docs/design/repo-layout.md](docs/design/repo-layout.md) — **minicore
  소스 저장소 자체**의 디렉토리 구조 (`kernel/`, `servers/`, `libc/` 등).
- [docs/spec/vfs-layout.md](docs/spec/vfs-layout.md) — **minicore가
  부팅한 뒤 유저에게 보이는 런타임 파일시스템 계층**
  (`/sys`, `/usr/{사용자명}`, `/home` 등).

## 실행 원칙

- 실제 파일 스캐폴딩(디렉토리·CMake 생성 등)은 `docs/plan`에 계획을
  먼저 남기고, 실행한 뒤 `docs/done`에 결과를 기록하는 순서를 따른다.
- 크로스 툴체인은 저장소 안에 vendoring하지 않는다(ADR-031) — 저장소
  밖에서 빌드/설치하고 PATH 또는 CMake 캐시 변수로만 참조한다.
- 검증 결과는 정직하게 보고한다 — 실제로 확인하지 못한 것을 확인했다고
  기록하지 않는다(예: 크로스 컴파일러가 없는 환경에서는 "컴파일러를
  찾지 못해 실패"까지만 확인하고, 그 이상을 검증했다고 주장하지 않는다).

## 시작점

- 설계 결정 색인: [docs/design/index.md](docs/design/index.md) (ADR-001~, 주제별 파일 안내)
- 모든 문서 색인: [docs/index.md](docs/index.md)
- [docs/plan/kernel-bootstrap.md](docs/plan/kernel-bootstrap.md)(M1~M8)과
  [docs/plan/smp-fpu-bringup.md](docs/plan/smp-fpu-bringup.md)
  (M9~M11b) 전체가 완료됨 — 결과는 [docs/done/](docs/done/) 참고.
  [docs/plan/system-servers-bringup.md](docs/plan/system-servers-bringup.md)
  (M12: 부트 디바이스 마운트+procsrv ~ M20: libc 포팅+로그인 후 셸)
  **전체(M12~M20)가 완료됨** — 결과는
  [docs/done/system-servers-bringup-m12-full.md](docs/done/system-servers-bringup-m12-full.md),
  [docs/done/system-servers-bringup-m13.md](docs/done/system-servers-bringup-m13.md),
  [docs/done/system-servers-bringup-m14.md](docs/done/system-servers-bringup-m14.md),
  [docs/done/system-servers-bringup-m15.md](docs/done/system-servers-bringup-m15.md),
  [docs/done/system-servers-bringup-m16.md](docs/done/system-servers-bringup-m16.md),
  [docs/done/system-servers-bringup-m17.md](docs/done/system-servers-bringup-m17.md),
  [docs/done/system-servers-bringup-m18.md](docs/done/system-servers-bringup-m18.md),
  [docs/done/system-servers-bringup-m19.md](docs/done/system-servers-bringup-m19.md),
  [docs/done/system-servers-bringup-m20.md](docs/done/system-servers-bringup-m20.md)
  참고(M14의 실제 USB 장치 열거/HID, M16의 FAT32/ext4 쓰기·하위
  디렉터리, M17의 멀티 TTY·세션 프로그램 스폰, M18의 jail 실제
  오버레이·cfgsrv 위임 저장·임의 명령 su/sudo, M19의 group 권한
  검증·재부팅을 넘어서는 실제 영속화·진짜 커널 reg_table 객체,
  M20의 **실제 서드파티 libc/셸 포팅(계획이 원래 요구한 것 —
  ADR-170에 따라 minicore 전용 대체 셸+libmc 최소 부분집합으로
  기능적 완료 기준만 충족)**·외부 프로그램 fork/exec·계정별
  session_program은 범위 밖으로 명시).
  **이 계획 문서에는 다음 마일스톤이 없다** — 다만 M20이 남긴
  "실제 libc/셸 포팅"은 프로젝트 자체의 완료를 뜻하지 않으며 별도
  후속 계획 대상이다.
  OPEN-29·OPEN-50·OPEN-53·OPEN-55~59·OPEN-61은 이미 해소돼 있다.
- M20 완료 후, 어떤 계획에도 속하지 않는 별도 확인 작업으로 **실제
  부팅 경로 둘 다(GRUB Multiboot2, UEFI/OVMF) 처음 검증**했다 —
  결과와 그 과정에서 발견·수정한 버그(레거시 8259 PIC, initrun/devmgr
  arch_data_addr)는
  [docs/done/real-hardware-boot-verification.md](docs/done/real-hardware-boot-verification.md)
  참고. aarch64 부팅 경로는 아직 커널 코드 자체가 없어(ADR-009,
  x86_64 완주 후 이식 예정) 이번 확인 대상이 아니다 — 사용자가
  "일단 생략, aarch64보다 먼저 범용 OS로서 미비된 부분을 보완하자"
  로 방향을 정했다.
- 그 방향에 따라 [docs/plan/general-purpose-completion.md](docs/plan/general-purpose-completion.md)
  (M21 선점형 스케줄링 ~ M26 실제 libc 포팅 재도전)를 계획했고
  **전체(M21~M26)가 완료됐다** — 이 계획에는 더 이상 다음
  마일스톤이 없다. 각 라운드 결과(전부 "이번엔 범위를 좁힌다"는
  M17~M20의 패턴을 반복함):
  M21([done](docs/done/general-purpose-completion-m21.md)) LAPIC
  타이머 기반 선점(BSP·ring3 한정, ADR-176)+TSS.RSP0 스레드별
  분리(ADR-177) · M22([done](docs/done/general-purpose-completion-m22.md))
  `sys_process_kill`(ADR-178)+procsrv의 wait/kill 자기테스트 ·
  M23([done](docs/done/general-purpose-completion-m23.md))
  `sys_fork`의 handle_table 복제(ADR-179)+fork+exec fd 상속
  자기테스트 · M24([done](docs/done/general-purpose-completion-m24.md))
  `sys_brk`+libmc 범프 할당자(ADR-180) · M25([done](docs/done/general-purpose-completion-m25.md))
  virtio-net 드라이버+netsrv DHCP 왕복(ADR-181, ARP 없이,
  `tools/smoke-test-net-x86_64.sh` 신설) · M26([done](docs/done/general-purpose-completion-m26.md))
  third_party/musl(첫 실제 git submodule) 문자열 함수 부분집합
  실제 포팅(ADR-182, 전체 syscall 계층은 범위 밖 — OPEN-66).
  다음 방향은 aarch64 이식이다(사용자가 이 계획 완료 후로 미뤄 둔
  것). **주의**: `servers/*`/`userland/*` 코드를 고친 뒤에는 일반
  `cmake --build`만으로는 `bootdisk.img`가 갱신되지 않는다
  (add_custom_target이라 기본 빌드에 안 걸림) —
  `--target minicore_bootdisk_image`를 반드시 추가로 돌려야 한다
  (M22에서 실제로 겪음, docs/done/general-purpose-completion-m22.md
  참고).
- M26 완료 후, 사용자가 OPEN 항목 검토 + 실제로 동작하는 musl 포팅
  (동적 링킹·멀티코어 선점·pthread·locale·완전한 signal·외부 SDK
  포함)을 다음 목표로 지정했다. 계획은
  [docs/plan/real-libc-syscall-layer.md](docs/plan/real-libc-syscall-layer.md)
  (M27~M39, **아직 착수 전**) — 핵심 전략: 커널을 확장하지 않고
  musl의 `syscall_arch.h`를 패치해 유저랜드(`libmc` 경유, ADR-183)로
  우회 · 동적 링킹을 M29로 앞당겨 이후 마일스톤 전부의 기본 검증
  경로로 씀(ADR-189, 최초 초안이었던 "맨 뒤 스트레치" 대비 개정) ·
  LAPIC 타이머 보정+`timer_source_interface` 추상화(ADR-184/191, OPEN-62)
  로 진짜 멀티코어 선점(ADR-185, OPEN-63) · signal(ADR-186, OPEN-65
  포함 해소)·pthread(ADR-187)·locale(ADR-188) · 저장소 밖 SDK
  내보내기(ADR-190) · 새 와이어 프로토콜은 서버 헤더의 `@wire-op`
  마크업+추출 도구부터 만들고 시작(ADR-195, OPEN-54). OPEN-64는
  절반만 겨냥, OPEN-66은 완료돼도 완전히 해소되지 않는다 — 세부는
  계획 문서와 [docs/design/open-items.md](docs/design/open-items.md)
  참고.
- 이 계획을 세우며 `docs/reply.md`에 쌓여 있던 미결정 답변들도
  함께 반영·처리했다 — OPEN-42(명령 단위 범위)는 ADR-194, OPEN-52
  (준비완료 신호 통일)은 ADR-193으로 해결됐다. OPEN-51(유저 서비스
  관리자 데몬의 정체성)은 **ADR-192(2026-09-10 사용자 피드백으로
  갱신)** — 데몬 자체는 폐기하지 않는다: "커널 서버"(procsrv/vfs/
  devmgr 등)는 initrun이 하드코딩된 이름으로 직접 실행하고, 이
  systemd류 데몬(`servers/svcmgr`)은 그와 겹치지 않는 "유저
  서비스"만 관리한다. 프로세스 트리의 영구 루트는 이 데몬이다.
- OPEN-51이 남긴 "유저 서비스를 실제로 어떻게 등록·시작·정지하는가"
  도 이어서 설계했다 — 유닛 모델+시작 절차+컨트롤 프로토콜 개요는
  [ADR-196](docs/design/boot-and-drivers.md), 레지스트리 스키마
  (`@global/system/services`, 새 프로토콜 없이 기존 `reg_op` 재사용)
  는 [ADR-197](docs/design/registry-decisions.md). 실제 구현 계획은
  [docs/plan/user-service-manager.md](docs/plan/user-service-manager.md)
  (M40 `servers/svcmgr` 골격+재부모화 완성 ~ M43 계정별 인스턴스,
  스트레치) — **아직 착수 전**. svcmgr는 순수 minicore 네이티브
  서버(`libk`+`libmc`만)라 `real-libc-syscall-layer.md`와 독립적으로
  병행 가능하고, M27(재부모화 메커니즘)만 선행 전제다. **OPEN-60**(cfgsrv I/O
  권한 동적 부여)은 여전히 미해결이다 — 한때 같은 번호로 태그됐던
  답변("fork 특수 변형 한정")은 태그 오기였음을 사용자가 확인했다
  (2026-09-10, M32/M37의 확인사항으로만 반영).
- 네임스페이스 컨벤션도 정리했다 — 커널(및 커널과 함께 컴파일되는
  코드)은 `kern::*` 계층(`kern::arch::x86_64`/`kern::ipc`/`kern::mm`
  등, 서브시스템당 하나), 커널 서버는 `kernsrv::<서버명>`, 외부
  비노출 하위 네임스페이스는 `__internals__`, 프로토콜(와이어 포맷)
  정의는 예외로 `kern::proto`/`kernsrv::proto`
  ([ADR-198](docs/design/foundations.md), [cxx-conventions.md](docs/spec/cxx-conventions.md)
  §6). [docs/plan/namespace-refactor.md](docs/plan/namespace-refactor.md)
  (M44~M48)로 **실제 적용까지 전부 완료됐다**(결과는 done 참고,
  이 계획에는 더 이상 다음 마일스톤이 없다) — `object`/`ipc`/`mm`/
  `sched`/`klog`/`initrd`→`kern::*`(M44), `arch_x86_64`→
  `kern::arch::x86_64`(M46, `kern::proc`은 비용 대비 가치 부족으로
  안 만듦), 서버 14개 전부 `kernsrv::<이름>`으로 감쌈(M47),
  netsrv의 IEEE/IANA/RFC 표준 상수 5개를 `kernsrv::proto`로
  분리(M48). M45(`uapi`→`kern::proto` 단순 리네임)는 아래 라이브러리
  재배치 작업(M50)으로 대체돼 스킵했다. 매 마일스톤 빌드+QEMU
  5개 스위트 회귀 없음 확인. 실행 중 발견한 것: `boot_info.hpp`가
  이미 initrun과 공유되는 ABI라 `kern::` 대상이 아님(M44),
  `process_ops.*`는 분리 비용이 커서 `kern::arch::x86_64`에 유지
  (M46), netsrv엔 애초에 "헤더 구조체"가 없었음(M48).
- 이 저장소가 직접 만들고 유지·관리하는 라이브러리(`libk`/`libmc`)
  경로와 명명 규칙도 정리했다 — `libs/` 하위로 옮기고 `lib` 접두사를
  뗀다(`libk`→`k`→`libs/k/`, `libmc`→`mc`→`libs/mc/`,
  [ADR-199](docs/design/build-system.md)). 더 나아가 `mc`를
  **커널·유저 공용**으로 통합한다 — 소비자가 정의하는 전처리기
  매크로(`MC_LAND_KERNEL`)로 커널-랜드/유저-랜드를 가르고, 커널
  syscall ABI를 `mc`의 헤더로 흡수해 `uapi.hpp`를 폐지한다
  ([ADR-200](docs/design/foundations.md), ADR-132 보강).
  [docs/plan/libs-restructure.md](docs/plan/libs-restructure.md)
  (M49 이동+리네임, M50 `uapi.hpp` 폐지+매크로 도입 — M45를 대체)
  **전체(M49~M50)가 완료됐다** — 결과는
  [docs/done/libs-restructure-m49.md](docs/done/libs-restructure-m49.md),
  [docs/done/libs-restructure-m50.md](docs/done/libs-restructure-m50.md)
  참고. `kernel/include/uapi.hpp`는 실제로 삭제됐고, 저장소는
  이제 [repo-layout.md](docs/design/repo-layout.md)의 목표 트리
  (`libs/k/`, `libs/mc/`) 그대로다. M50 실행 중 발견한 것: `uapi::message`
  등 옛 구조체는 필드별 NSDMI가 있어 비트리비얼 타입이었지만 새
  순수 C `mc_message` 등은 트리비얼 aggregate라 `{}` zero-init이
  `memset` 호출로 낮춰져 유저랜드(이전엔 `minicore_libmc`를 링크한
  적이 없었다)에 `undefined symbol: memset` 링크 에러가 났다 —
  `libs/mc/src/freestanding_mem.c` 신설+유저 실행파일 15개 전부에
  `minicore_libmc` 링크 추가로 해결(커널은 자신의
  `freestanding_mem.cpp`가 있어 `minicore_libmc`를 링크하지 않으므로
  중복 심벌 없음). 이 계획에는 더 이상 다음 마일스톤이 없다.
- `namespace-refactor.md`와 `libs-restructure.md` 둘 다 완료됐다.
  다음 계획으로 [docs/plan/user-service-manager.md](docs/plan/user-service-manager.md)
  (M40~M43)를 시작하려 했으나, 그 계획의 M40이 자체적으로
  [docs/plan/real-libc-syscall-layer.md](docs/plan/real-libc-syscall-layer.md)
  **M27**(procsrv 실제 프로세스 테이블+범용 OP_WAIT/OP_KILL+재부모화
  메커니즘)이 먼저 끝나 있어야 한다고 명시하고 있어 실행 순서와
  충돌이 발견됐다 — 사용자가 **순서를 뒤집어 real-libc-syscall-layer.md
  (M27~M39) 전체를 먼저 끝내고, 그 다음 user-service-manager.md
  (M40~M43)를 진행**하기로 확정했다(2026-09-10).
  [real-libc-syscall-layer.md](docs/plan/real-libc-syscall-layer.md)의
  **M27~M29가 완료됐다(M29는 부분 완료)**(결과는
  [docs/done/real-libc-syscall-layer-m27.md](docs/done/real-libc-syscall-layer-m27.md),
  [docs/done/real-libc-syscall-layer-m28.md](docs/done/real-libc-syscall-layer-m28.md),
  [docs/done/real-libc-syscall-layer-m29.md](docs/done/real-libc-syscall-layer-m29.md),
  [ADR-201](docs/design/security-model.md)/[ADR-202](docs/design/kernel-memory.md)/
  [ADR-203](docs/design/kernel-memory.md) 참고) — M27: procsrv 실제
  process_entry 테이블+범용 `proc_op::wait`/`kill`(caller_pid
  자기주장+비블로킹 폴링으로 범위 좁힘, OPEN-54 해소+OPEN-67 신설)+
  재부모화 메커니즘 증명. M28: `tools/apply-patches.sh` 실제 구현+
  musl의 syscall_arch.h 패치(모든 syscall을 `libc/sysdeps/minicore/
  syscall_shim.c`로 우회)+`userland/musl-hello`가 musl의 진짜 시작
  경로로 진입해 "hello from real musl" 출력 — 실행 중 musl의
  `__init_tls`가 `arch_prctl`(FS_BASE)을 무조건 요구하고 Linux ABI
  초기 스택(argc/argv/envp/auxv)도 필요함을 발견해, 원래 M30/M29
  계획이던 두 커널 기능을 M28로 앞당겼다(ADR-202). M29(부분 완료):
  커널 ELF 로더의 ET_DYN+load_bias 지원+진짜 AT_PHDR/AT_ENTRY/
  AT_BASE auxv 구성까지는 완료했지만, musl 자신의 `libc.so`
  (ldso/dynlink.c 2439줄, 전체를 -fPIC로 재컴파일한 멀티콜
  바이너리+자기재배치 부트스트랩)를 실제로 빌드해 동작시키는 것은
  M27/M28과 질적으로 다른 위험도라 판단해 시도하지 않았다 — 계획
  문서 자신이 명시한 "크게 막히면 정적 링킹 복귀" 조항을 실행했다
  (ADR-203). M30(완료): musl 자신의 실제 malloc(`lite_malloc.c`,
  `SYS_mmap` 기반)이 M26의 `mem_shim.c`를 대체 — 착수 전 분석으로
  musl malloc이 우선 시도하는 `SYS_brk`가 `libmc`의 `mc_malloc`
  (셸이 직접 호출)과 같은 커널 힙 상태를 공유해 겹칠 수 있음을
  발견해, `SYS_brk`를 항상 실패시켜 완전히 분리된 `SYS_mmap` 전용
  영역으로 우회시켰다(ADR-204, 양쪽 소스 모두 무수정).
  musl-hello가 malloc+free 왕복과 errno==EBADF까지 QEMU로 확인.
  M31(완료): 파일 I/O syscall(SYS_open/openat/read/readv/close/
  writev, 새 `libmc` `mc_fs_read` 단발 읽기)+진짜 musl stdio
  (fopen/fread/fclose/printf, 재구현이 아니라 musl 소스 자체)를
  musl-hello가 처음 실전에 씀 — VFS 핸들이 필요해 musl-hello를
  커널 직접 스폰에서 initrun의 15번째 정식 서비스로 재배치했다
  (새 `mkbootdisk.py --linux-abi-stack=` ini 키, ADR-205).
  **M32(완료)**(결과는
  [docs/done/real-libc-syscall-layer-m32.md](docs/done/real-libc-syscall-layer-m32.md),
  [ADR-206](docs/design/security-model.md)/[ADR-207](docs/design/kernel-memory.md)
  참고): 진짜 musl `fork()`+`execve()`+`waitpid()`+`getpid()` 실왕복.
  procsrv에 `self_register`/`fork_register`를 추가해 M27의 "procsrv
  직접 스폰 프로세스 사이에서만" 제약을 initrun이 스폰한 일반
  프로세스까지 확장하고, pid를 유저랜드 static이 아니라 커널 스레드
  필드(`thread::procsrv_pid`)에 저장해 `execve()`를 거쳐도 보존시켰다
  (ADR-206) — 자식이 곧바로 exec()하는 이 라운드의 실제 시나리오상
  static 캐시는 exec가 BSS를 통째로 새로 갈아엎어 사라진다는 것을
  실제로 겪었다. 자식이 execve()로 **다른** 이미지(신규
  `userland/musl-exec-target`, `tools/bin2c.py`로 procsrv 자신에
  컴파일 시점 데이터로 심어 VFS(memfs)에 씀)를 실행하고 부모가 그
  exit code를 회수함을 확인했다(M29가 이미 ADR-203으로 정적 링킹에
  되돌아가 있어, PT_INTERP 로더 경로 확인은 이번에도 범위 밖).
  실행 중 `fork_current()`가 `fs_base`(TLS)를 물려주지 않아 musl
  자식이 exec 전에 죽는 진짜 버그를 발견해 고쳤다(ADR-207,
  `io_port_base`/`count`와 같은 자리에 한 줄 추가) — musl 프로그램이
  `fork()`한 것은 이번이 처음이라 지금까지 드러나지 않았던 간극이다.
  **M33(완료)**(결과는
  [docs/done/real-libc-syscall-layer-m33.md](docs/done/real-libc-syscall-layer-m33.md),
  [ADR-208](docs/design/kernel-scheduler.md) 참고, OPEN-62 해소):
  HPET(1순위)/PIT(폴백) 실측으로 LAPIC 타이머를 코어마다(BSP+각 AP)
  보정하는 `calibrate_lapic_timer()`를 만들었다. 실행 중 발견:
  `ticks_for()`(scheduler.cpp)가 계산된 initial_count와 무관하게
  `base_time_slice_us`를 그대로 "틱 수"로 소비해 왔다는 것을
  발견해, 진짜 나눗셈으로 고쳐야 OPEN-62가 완전히 해소됨을
  확인했다. ADR-191이 예정한 `timer_source_interface` 추상화는
  소비자가 BSP 하나뿐인 이 시점엔 조기 추상화라 판단해 M34(진짜
  멀티코어 선점)로 미뤘다.
  **M34(완료)**(결과는
  [docs/done/real-libc-syscall-layer-m34.md](docs/done/real-libc-syscall-layer-m34.md),
  [ADR-209](docs/design/kernel-scheduler.md) 참고, OPEN-63 해소):
  진짜 멀티코어 선점형 스케줄러 — `g_current`를 코어별 배열로 바꾸고,
  AP가 유저 밴드에서 직접 유저 스레드를 뽑아 실행하며(커널 밴드
  데모는 BSP만, 멀티코어 검증 이력이 없어 의도적으로 제외), 각
  코어가 자기 LAPIC 타이머로 독립 선점한다. 실행 중 BSP 하나만
  유저모드를 실행하던 시절엔 절대 드러날 수 없었던 진짜 멀티코어
  버그 3건을 QEMU로 연달아 재현·수정했다: (1) `libk::irq_safe<Lock>`
  이 뮤텍스를 잡기 **전에** 공유 필드에 irq 상태를 저장하던 순서
  버그(run_queue.lock이 처음 실제 경합에 들어가며 두 코어가 서로의
  저장값을 덮어써 한 코어가 다시는 깨어나지 못함), (2) SYSCALL
  진입이 읽는 커널 스택 포인터가 전역 하나뿐이었음(swapgs+
  IA32_KERNEL_GS_BASE로 코어별 슬롯 분리), (3) TSS/GDT가 전역
  하나뿐이었음(x86_64는 코어마다 별도 TSS가 필요 — AP에서 유저
  스레드가 첫 타이머 인터럽트를 받는 순간 TSS를 못 찾아 트리플
  폴트로 조용히 멈췄다, 코어별 사설 GDT+TSS로 분리해 고침).
  `timer_source_interface`(ADR-191)는 여전히 미뤘고, `handle_table`
  무동기화(ADR-136, M11부터 이미 지적)도 이번 라운드는 해소하지
  않았다(OPEN-68 신규 등록 — busy/counter 데모가 IPC를 안 써서
  이 경로를 안 건드림).
  **M35(완료)**(결과는
  [docs/done/real-libc-syscall-layer-m35.md](docs/done/real-libc-syscall-layer-m35.md),
  [ADR-210](docs/design/foundations.md) 참고): musl `setlocale()`
  왕복 — "C"/"POSIX" 고정 검증. 실행 중 발견: 계획 문서의 "미지원
  로케일(`ko_KR.UTF-8`)은 실패해야 한다"는 실제 musl 동작과 다름을
  소스 확인으로 발견했다(musl은 알 수 없는 로케일도 실패시키지
  않고 C.UTF-8로 조용히 대체한다) — 검증 목표를 "요청은 성공하지만
  ctype 동작은 여전히 C"로 조정했다.
  **M36(완료, 범위 재좁힘)**(결과는
  [docs/done/real-libc-syscall-layer-m36.md](docs/done/real-libc-syscall-layer-m36.md),
  [ADR-211](docs/design/kernel-scheduler.md) 참고): 완전한 signal
  계층 — `pending_signals`/`signal_mask`/`sigactions[32]`+새 syscall
  3개(`sys_signal_action`/`sys_signal_send`/`sys_rt_sigreturn`)+
  `syscall_entry.S`의 return-to-user 훅. 계획 단계 ADR-186의 6개
  결정 중 syscall 리턴 시점 전달만 실제로 구현했다 — IRETQ 경로,
  `SIGCHLD` 자동 전달, `SIGKILL` 즉시 대기열 unlink는 전부 범위
  밖으로 남겨 **OPEN-65를 다시 열었다**(ADR-186 계획 단계에 앞당겨
  "해소" 표시가 돼 있었으나 실제로는 구현되지 않았음을 뒤늦게
  확인). 실행 중 진짜 버그 3건을 QEMU로 재현·수정했다: (1)
  `libc/CMakeLists.txt`의 musl include 검색 순서가 musl 원본
  Makefile과 반대라 arch별 `ksigaction.h` 오버라이드가 안 먹혀
  `sigaction.c`가 존재하지 않는 `__restore` 심벌을 참조(순서를
  원본과 맞춰 고침), (2) musl의 `restore.s`(핸들러가 `ret`한 뒤
  CPU가 곧바로 뛰어드는 손짜기 트램폴린 — `__syscallN` 우회를
  전혀 거치지 않는다)가 진짜 Linux ABI(syscall 번호를 RAX에 싣는다)
  를 그대로 써 이 커널의 RDI 기반 syscall ABI와 맞지 않아
  `sys_rt_sigreturn`이 전혀 실행되지 않고 핸들러가 남긴 임의의 RDI
  값을 번호로 오인해 `#GP`로 죽음(`third_party/patches/musl/
  0002-restore-trampoline.patch`로 그 한 줄만 이 커널의 번호를
  쓰도록 고침 — 0001 다음 이 패치 파이프라인의 두 번째 실사용), (3)
  자기테스트 자체의 fork 스케줄링 경합(부모가 자식의 `sigaction()`
  등록보다 먼저 `mc_signal_send()`를 보내면 "SIG_DFL=무시"
  단순화가 그 신호를 조용히 버림, `getpid()`처럼 pid가 이미
  캐시된 syscall은 스케줄러를 안 건드려 재시도의 "쉬는 시간"으로
  못 씀 — 새 syscall `sys_yield`(`kern::sched::yield()`를 유저랜드에
  노출, musl `sched_yield()`가 우회)로 해결).
  **M37(완료)**(결과는
  [docs/done/real-libc-syscall-layer-m37.md](docs/done/real-libc-syscall-layer-m37.md),
  [ADR-212](docs/design/kernel-scheduler.md) 참고): pthread 최소
  구현 — 새 syscall `sys_thread_create`(owner_space/handle_table을
  fork처럼 클론하지 않고 그대로 공유)+`sys_futex`(WAIT/WAKE만)+
  `address_space::heap_lock`(brk/mmap_anon 보호, ADR-180이 선행
  조건으로 미리 지적해 둔 스핀락). musl 자신의 진짜
  `pthread_create()`/`pthread_join()`/`pthread_mutex_*`가 처음
  링크됐다 — 워커 둘이 mutex로 보호된 공유 카운터를 각 10만 번씩
  증가시킨 뒤 join해 정확한 합계(20만)를 확인. 계획(ADR-187) 대비
  범위를 더 좁히지 않았지만, 실행 중 진짜 문제 3건을 발견했다:
  (1) musl의 `__clone`(hidden asm)도 M36의 `__restore_rt`와 같은
  이유(raw Linux ABI 직접 사용)로 이 커널의 syscall ABI와 안 맞아
  순수 C 대체(`libc/sysdeps/minicore/clone_shim.c`)로 완전히 갈아
  끼웠다 — 한 줄 패치로 안 끝났다(clone()의 "부모와 같은 명령어
  스트림을 이어 간다"는 의미론 자체가 이 커널의 `sys_thread_create`
  (새 스레드가 처음부터 entry_rip로 곧바로 진입)엔 필요 없어서,
  musl이 넘기는 stack 인자의 16바이트 정렬 보정까지 대체 코드에
  그대로 옮겨야 했다), (2) `syscall_shim.c`가 M32부터
  `SYS_exit`/`SYS_exit_group`을 한 케이스로 묶어 둔 게 진짜 회귀
  버그였다 — 단일 스레드 프로세스만 있던 M27~M36까지는 드러나지
  않았지만, pthread 하나가 끝날 때(raw `SYS_exit`)마다 프로세스
  전체 종료로 procsrv에 잘못 보고될 뻔했다(진짜 프로세스 종료는
  항상 `SYS_exit_group`을 쓴다) — 분리해 고쳤다, (3) M30/M31이
  "아직 스레드가 하나뿐"이라는 이유로 no-op으로 미뤄 둔
  `__lock`/`__unlock`을, 두 pthread의 `pthread_exit()`이 거의
  동시에 끝나는 시나리오에서 musl의 스레드 목록이 깨질 수 있다는
  것을 QEMU로 재현하기 전에 소스 분석으로 먼저 발견해, 진짜 futex
  기반(musl 원본 `__lock.c`)으로 되돌렸다.
  **M38(완료)**(결과는
  [docs/done/real-libc-syscall-layer-m38.md](docs/done/real-libc-syscall-layer-m38.md),
  [ADR-213](docs/design/build-system.md) 참고): minicore 타깃 SDK
  내보내기 — `tools/export-sdk.py`+`tools/sdk-template/`(공용
  link.ld/컴파일러 래퍼/CMake 툴체인 파일). 계획(ADR-190) 대비
  범위 조정 2건: (1) 동적 `libc.so`는 내보내지 않는다(M29/ADR-203
  이 이미 정적 링킹으로 되돌아가 있어 존재하지 않는 산출물이었다,
  계획 문서를 다시 읽고서야 이 불일치를 알아챘다) (2) 타깃
  트리플을 계획의 `x86_64-linux-musl`이 아니라 이 저장소의 `libc.a`
  자신이 실제로 컴파일된 `x86_64-unknown-none-elf`로 확정했다.
  실행 중 발견: 컴파일러 래퍼(bash 스크립트)를 `CMAKE_C_COMPILER`
  로 직접 지정하면 이 세션의 Windows 호스트에서 ninja가 `%1 is not
  a valid Win32 application`으로 실패했다(cmake/ninja는 컴파일러를
  셸을 거치지 않고 직접 실행한다) — `x86_64-minicore.cmake`를
  `toolchain/x86_64-clang.cmake`(ADR-020)와 같은 방식(clang을
  `CMAKE_C_FLAGS_INIT`으로 직접 감싼다)으로 바꿔 해결했다. 검증:
  저장소 밖 스크래치 디렉터리에서 minicore 소스를 전혀 참조하지
  않는 순수 C "hello world"(printf+malloc+strcpy)를 이 SDK만으로
  CMake+ninja로 컴파일·링크했고, 결과 ELF를
  `tools/mkbootdisk.py`(수정 없이 그대로 받음 — ADR-190이 미리
  걸어 둔 확인 항목)로 임시 부트디스크에 넣어 `MINICORE_QEMU_BOOTDISK`
  로 QEMU에서 부팅해 "hello from minicore SDK (23 bytes)"가 정확히
  출력됨을 확인했다(이 검증은 일회성 증명이라 저장소의
  `servers/CMakeLists.txt`에는 편입하지 않았다). 새 커널/유저랜드
  코드 변경이 없어 QEMU 5개 회귀 스위트는 다시 돌리지 않았다(M37
  완료 시점의 확인이 유효).
  **M39는 스킵했다**(2026-09-10, 사용자 결정) — 계획 문서 자신이
  "실패해도 M27~M38의 성과는 독립적으로 유효하다"고 명시한
  스트레치 목표이고, job control/파이프 리다이렉션처럼 계획이 이미
  범위 밖으로 못박아 둔 것들이 실제로 필요해질 가능성이 높아(새
  서드파티 셸 서브모듈+정적 링킹 기반 포팅, 상당한 추가 시간이
  드는 멀티스텝 작업) 사용자가 이 시점에 user-service-manager.md
  로 바로 넘어가기로 정했다. real-libc-syscall-layer.md는 이제
  M27~M38(완료)+M39(스킵)로 마무리됐다 — 다음은
  [docs/plan/user-service-manager.md](docs/plan/user-service-manager.md)
  M40(`servers/svcmgr` 골격+재부모화 완성)부터 시작했다.
  **M40(완료)**(결과는
  [docs/done/user-service-manager-m40.md](docs/done/user-service-manager-m40.md),
  [ADR-214](docs/design/boot-and-drivers.md) 참고): 새 서버
  `servers/svcmgr`(libk+libmc만 링크, initrun이 `--service=` 목록의
  마지막 항목으로 spawn) — procsrv 새 wire op `adopt_orphans`
  (재부모화 실제 트리거, M27이 잠정 처리해 둔 `parent_pid=
  k_parent_none`을 실제 svcmgr pid로 교체)+`mc/lifecycle_client.h`
  (신규, `mc_signal_ready`/`mc_wait_ready`, ADR-193의 준비완료
  신호를 처음 실제로 구현)+하드코딩된 데모 유닛 하나
  (`userland/svcmgr-demo-unit`, VFS 없이 svcmgr가 자신의 컴파일
  시점 데이터로 직접 spawn). 계획(ADR-192/193/196) 자체의 방향은
  안 바뀌었지만, 처음 이 경로를 쓰는 소비자가 나타나며 진짜 버그
  4건을 발견했다: (1) `create_endpoint=true`가 주는 프록시 핸들이
  M22부터 CAN_SEND만 있어(부모→자식 방향만 가정) 준비완료 신호
  (방향이 반대 — 자식→부모)를 못 받음 — CAN_RECV를 추가(기존 경로는
  그대로 유지), (2) `mc/vfs_client.h`/`fs_client.h`/`console_client.h`/
  `ps2_client.h`/`procsrv_client.h` 다섯 헤더 전부에 `extern "C"`
  가드가 없어, 이 계층의 함수를 처음 직접 호출한 C++ 소비자
  (svcmgr)의 링크가 이름 맹글링으로 깨짐 — 다섯 헤더 전부(+신규
  `lifecycle_client.h`)에 추가, (3) `--depends=`에 커널 서버 15개를
  전부 나열했다가 `MC_MAX_SPAWN_INHERITED_HANDLES`(=4)와 initrun의
  depends= 파싱 버퍼(96바이트)를 동시에 넘어 **핸들 상속 전체가
  조용히 무시**됨(procsrv 핸들조차 못 받아 self_register가 즉시
  실패) — 스폰 순서 보장(`--service=` 목록에서 마지막 줄이라는
  사실 자체로 이미 충족, `depends=`와 무관)과 핸들 상속(별개
  메커니즘, 실제 필요한 건 procsrv 하나)을 혼동한 것이었다 —
  `--depends=svcmgr:procsrv` 하나로 줄여 해결, (4) 실제 부팅
  경로에서는 어떤 커널 서버도 procsrv에 self_register한 적이 없어
  (procsrv 자신의 pid=1 등록조차 self-test 전용이었다) 재부모화를
  관찰할 실제 대상이 하나도 없었음 — procsrv가 `_start()` 맨 앞에서
  무조건 자기 자신을 pid=1로 등록하도록 고쳐 최소 하나의 관찰
  가능한 대상을 만들었다.
  **M41(완료)**(결과는
  [docs/done/user-service-manager-m41.md](docs/done/user-service-manager-m41.md),
  [ADR-215](docs/design/registry-decisions.md) 참고): svcmgr가
  하드코딩 유닛 목록을 실제 `@global/system/services` cfgsrv
  테이블로 대체 — 새 `mc/cfgsrv_client.h`(재사용 가능한 레지스트리
  클라이언트, procsrv의 M19 self-test가 인라인으로만 쓰던 것을
  처음 뽑음)+`mc/svcmgr_protocol.h`(`mc_svcmgr_service_unit`).
  M42의 `op_register`가 아직 없어 테이블이 비어 있으면 svcmgr
  자신이 자기테스트 유닛 둘(`svc-b`가 `svc-a`에 `depends_on`)을
  등록, `depends_on` 위상정렬로 `svc-a`→`svc-b` 순서를 확인하고
  `delete_value`+재조회로 삭제도 확인했다(계획의 "재부팅 후 확인"
  중 재부팅 부분은 cfgsrv 저장 파일이 기본 memfs라 진짜 재부팅을
  못 버텨 범위 밖으로 좁혔다 — 같은 부팅 안에서만 증명). `exec_path`
  (VFS 경로)는 저장/조회는 되지만 아직 읽지 않는다(모든 유닛이
  여전히 같은 임베딩된 데모 ELF를 실행 — VFS 쓰기 클라이언트가
  더 필요해 범위 밖). 실행 중 발견한 진짜 버그 4건, 그중 하나는
  실제 메모리 손상이었다: (1) 새 클라이언트의 페이지 버퍼에 정렬
  (`alignas`)이 없어 커널이 "page_descriptor not page-aligned"로
  즉시 패닉, (2) cfgsrv의 값 저장 한도가 256바이트뿐이라
  `service_unit`(~616바이트)이 매번 잘려 저장돼 다시 읽으면 길이가
  안 맞아 실패 — 1024로 상향, (3) cfgsrv의 영속화 버퍼(8KiB)가 그
  한도 상승 후 이론상 필요한 최대 크기(~64KiB)보다 훨씬 작아
  실제로 경계를 넘겨써 버퍼 뒤의 다른 정적 변수를 손상시켰다 —
  이 손상이 **완전히 무관해 보이는 "[shell] cat ok=0" 회귀**로
  처음 드러났다(다른 전역 상태가 오염된 결과) — 32KiB로 늘리고
  쓰기 전에 필요한 크기를 먼저 계산해 넘치면 아예 안 쓰는 방어
  코드를 추가, (4) 그 과정에서 fs-protocol에 close 오퍼레이션이
  애초에 없어(OPEN-70 신규 등록) 모든 VFS 소비자가 open할 때마다
  memfs의 열린 파일 슬롯을 영구히 소비한다는 것도 발견 — cfgsrv의
  매 저장마다의 새 open이 그 슬롯(16개)을 부팅 한 번 안에 실제로
  바닥내 무관한 shell의 cat 자기테스트까지 실패시켰다, 즉시는
  64로 늘려 막고 근본 수정(close 신설)은 OPEN-70으로 남김.
  **M42(완료)**(결과는
  [docs/done/user-service-manager-m42.md](docs/done/user-service-manager-m42.md),
  [ADR-216](docs/design/kernel-ipc-objects.md) 참고): svcmgr가
  자기 endpoint 위에서 컨트롤 프로토콜(`mc/svcmgr_protocol.h`,
  list/status/start/stop/restart/register/unregister)을 실제로
  처리한다 — 와이어 마크업 먼저(ADR-195, M27에 이은 두 번째 적용)
  달고 `tools/gen-wire-docs.py`로 [docs/spec/generated/svcmgr-wire.md](docs/spec/generated/svcmgr-wire.md)
  를 뽑았다. 별도 최소 검증 클라이언트(`userland/svcmgr-ctl-test`,
  계획이 "착수 시점에 확정"이라 남긴 자리)가 M41이 부팅 시 띄운
  svc-a를 stop→start로 왕복시키고 register로 svc-c를 추가한 뒤
  cfgsrv에 직접 물어 등록을 확인한다. op_stop/restart는 기존
  `sys_process_kill`(ADR-178), op_register/unregister는 M41의
  cfgsrv 클라이언트를 그대로 재사용한다(ADR-196 §결정7). 실행 중
  발견한 진짜 버그 3건: (1) initrun의 이름→핸들 레지스트리 크기
  (16)를 서비스 18개가 넘겨 svcmgr가 등록되지 못했고, ctl-test의
  고정 핸들 관례가 조용히 다른 서버(cfgsrv)를 가리켜 진짜
  페이지폴트로 죽음 — 32로 상향, (2) **가장 심각한 발견**:
  `thread::ipc.reply_target`이 스레드당 슬롯 하나뿐이라, op_start가
  아직 ctl-test의 호출에 회신하지 않은 채로 자식 프로세스의
  준비완료를 기다리는 재진입 `sys_recv`+`sys_reply` 왕복을 하면
  그 슬롯이 덮어써지고, 안쪽 회신이 그 슬롯을 비워 바깥쪽 회신이
  "대응하는 recv 없음"(ipc.md §3의 무동작 규칙)으로 조용히 사라지는
  진짜 교착이 났다 — M27~M41은 이 준비완료 대기 패턴을 항상 부팅
  시퀀스(메인 IPC 루프 시작 **전**)에서만 써서 한 번도 드러나지
  않았던 것이었다. 커널에 재진입 보존 스택을 추가해 해결했다
  (ADR-216 — `push_reply_target`/`pop_reply_target`, 스펙도
  `docs/spec/ipc.md` §3.2로 갱신), (3) 그 수정 후 op_register에서
  새 페이지폴트 — `handle_register`가 nested cfgsrv 호출 뒤까지
  들고 있던 IPC 수신 매핑 포인터가 이미 해제된 뒤였음(기존
  ADR-161 규칙을 svcmgr 코드가 어긴 것) — 수신 즉시 구조체 전체를
  로컬로 복사하도록 수정.
  **M43(완료, 스트레치)**(결과는
  [docs/done/user-service-manager-m43.md](docs/done/user-service-manager-m43.md),
  [ADR-217/218/219](docs/design/security-model.md) 참고): 계정별
  유저 서비스 인스턴스(systemd `user@.service` 대응). 사용자가
  방향을 명시했다 — 시스템 전역 설치 유저 서비스는 태생적 권한
  그대로(M40~M42와 동일), 계정 전용 서비스만 그 계정이 등록한
  **영구 위임**이 있어야(자가서비스로 등록·철회, 새 오퍼레이션
  없이 cfgsrv의 기존 set_value/delete_value 재사용) 로그인 시점에
  그 계정 몫으로 인스턴스화된다. svcmgr가 로그인 감시 백그라운드
  스레드(M37 `mc_thread_create` 첫 실사용)를 만들어 procsrv의 새
  오퍼레이션(`op_poll_login_event`/`op_spawn_delegated_unit`)을
  폴링하고, 컨트롤 프로토콜 주소 지정을 `"유닛@계정"`으로 확장했다.
  실행 중 발견한 진짜 버그/설계 오류 3건: (1) svcmgr가 procsrv에게
  이 민감한 새 오퍼레이션을 걸 때 "진짜 svcmgr"임을 증명할
  위조 불가능한 방법이 없었다(기존 오퍼레이션은 전부 자기주장 pid
  모델) — badge(ipc.md가 이미 설계해 뒀지만 M6~M42 내내 아무도
  실제로 쓴 적이 없던 필드)를 처음으로 스폰 시점 캐패빌리티 주입에
  연결했는데(ADR-217, svcmgr+procsrv 조합 전용 하드코딩 예약 badge),
  그 과정에서 `sys_recv`의 badge 반환값 자체가 M6부터 raw syscall
  계층에서 항상 버려지고 있었다는 것도 처음 발견해
  `kernel/arch/x86_64/syscall.cpp`에 out-포인터를 추가했다, (2)
  계정별 위임 테이블을 처음엔 `@global/system/service-delegates/
  <계정명>`에 두려 했으나, cfgsrv의 `normalize_path`/`schema_matches`
  가 `@global/...` 경로의 스키마를 항상 문자열 "global" 자체로
  고정 취급해 caller_uid!=0인 계정의 CREATE_TABLE이 절대 통과할
  수 없다는 것을 발견했다(uid=0/root만 `@global/*` 아래에 테이블을
  만들 수 있다는 의도된 설계) — 자가서비스 grant가 성립하려면 그
  계정 자신의 스키마(`@<계정명>/system/service-delegate`)로 옮겨야
  했다, (3) 로그인 감시 스레드 추가로 svcmgr가 처음으로 멀티스레드가
  돼 `g_runtime`을 두 스레드가 동시에 건드릴 수 있게 됐다 —
  스핀락(libk) 신설로 해결. OPEN-71(위임 기간 모드는 permanent
  하나뿐)/OPEN-72(스폰된 프로세스가 실제 커널/badge 수준의 계정
  신원을 안 받음)/OPEN-73(ADR-193 준비완료 핸드셰이크 미연결) 신규
  등록. **user-service-manager.md는 이제 M40~M43 전부 완료됐다** —
  이 계획에는 더 이상 다음 마일스톤이 없다.
- user-service-manager.md 완료 후, 실행되지 않고 남아 있던 유일한
  마일스톤(real-libc-syscall-layer.md M39, 사용자 결정으로 스킵)을
  사용자가 다시 다루기로 정했다. M39는 "동적 링킹으로 여러 바이너리가
  musl의 공유 `libc.so`를 나눠 쓴다"는 것을 전제하고 있었는데, 그
  전제 자체가 M29(ADR-203)에서 이미 무효화돼 있었다 — 그래서 M39를
  그대로 되살리지 않고, 정적 링킹 전제로 다시 설계한 새 계획
  [docs/plan/musl-userland-porting.md](docs/plan/musl-userland-porting.md)
  (M51 파이프+dup2 ~ M55 job control 최소, 스트레치)를 세웠다.
  BusyBox(sh+coreutils 단일 정적 바이너리)를 `third_party/`에 새
  submodule로 추가해 M20/M26/M39가 세 번 미룬 "실제 포팅된
  셸/coreutils로 로그인 후 셸 대체"를 이번에 달성하는 것이 목표다.
  **M51(완료)**(결과는
  [docs/done/musl-userland-porting-m51.md](docs/done/musl-userland-porting-m51.md),
  [ADR-220](docs/design/foundations.md) 참고): 익명 파이프
  (`pipe()`/`pipe2()`)+`dup2()` — 새 커널 프리미티브가 아니라
  ADR-183과 같은 전략(커널을 확장하지 않고 유저랜드 서버+syscall
  우회로 해결)으로, 새 서버 `servers/pipesrv`(procsrv/cfgsrv와 같은
  단일 요청-응답 루프, 절대 회신을 미루지 않는다)를 만들고
  `libc/sysdeps/minicore/syscall_shim.c`가 파이프가 비었거나
  가득 찼을 때 `mc_yield()`+재시도로 블로킹을 흉내낸다(`mc_wait()`
  가 이미 쓰는 것과 같은 요령, OPEN-67과 같은 이유). id는
  프로토콜-레벨 정수+참조 카운트(read_refcount/write_refcount) —
  `fork()`/`dup2()`로 같은 id를 여러 프로세스(또는 한 프로세스의
  fd 슬롯 여러 개)가 들고 있을 수 있는데 pipesrv는 이 복제를 스스로
  관찰할 수 없어, `syscall_shim.c`가 그 시점마다 명시적으로
  `op_dup`을 불러 참조 카운트를 알려주는 계약으로 풀었다. 새 실제
  musl 프로그램 `userland/pipe-test`가 (1) 같은 프로세스 안에서
  write→close→read→EOF (2) `fork()`로 파이프 양끝을 나눠 가진 뒤
  부모가 쓰고 자식이 읽는 왕복 (3) `dup2()`로 파이프 읽기 쪽을
  fd 0(stdin)에 덮어씌운 뒤 직접 읽기 — 세 시나리오를 확인했다.
  실행 중 발견: x86_64가 레거시 `SYS_pipe`(22)도 실제로 갖고 있어서
  (i386 전용이라고 잘못 가정했었다) musl의 `pipe()`가 계획이 미리
  준비해 둔 `SYS_pipe2`(293)가 아니라 `SYS_pipe`로 왔다 — 처음엔
  그대로 `-ENOSYS`로 떨어졌고, 둘 다 같은 핸들러로 처리하도록
  케이스를 합쳐 해결했다.
  **M52는 착수 중 방향을 바꿨고, 그 방향으로 완료됐다**
  (2026-09-11, ADR-221/222/223): BusyBox를 `third_party/busybox`
  (release `1_36_1`)로 vendoring까지는 됐지만, 빌드 연결 중 서로
  다른 두 층위의 환경 문제를 만났다 — (1) BusyBox의 Makefile
  (`scripts/trylink`)이 `$(CC)` 하나가 컴파일+링크를 다 하는
  정상적인 hosted gcc/clang을 전제하는데, 이 저장소는 정확히 반대
  (clang은 컴파일만, 최종 링크는 `ld.lld` 직접 호출, ADR-020/M38)라
  안 맞았다 — 이건 컴파일/링크 모드를 구분해 링크를 `ld.lld` 호출로
  바꿔치는 `CC` 셈 스크립트로 실제로 풀었다. (2) 그 뒤 Kconfig
  호스트 도구(`fixdep`)조차 이 MSYS2 설치의 호스트 `gcc`에 표준
  헤더 패키지(`msys2-runtime-*-devel`)가 없어 컴파일이 안 됐다 —
  패키지 설치로 고칠 수 있는 문제였지만, 사용자가 이 시점에서
  **BusyBox 도입 자체를 철회하고 셸/coreutils를 이 저장소에서
  직접 작성**하기로 결정했다(ADR-221). `third_party/busybox`
  submodule과 `tools/busybox-cc-shim.sh`는 되돌렸다. OPEN-74는
  "그 문제 자체가 더 이상 적용되지 않는 대상에 대한 것이었다"는
  뜻으로 ADR-221로 해소 처리했다. 그 결정을 실제로 완주했다 — 새
  `userland/msh`(진짜 fork()+execve()로 명령을 실행하는 최소 셸,
  빌트인 아님)+`echo`/`ls`/`cat`(coreutils, `ls`는 mc_fs_list를
  직접 호출). 실행 중 진짜 버그 2건을 발견·수정했다: (1)
  `execve()`가 M28부터 `argc=1`/고정 `argv[0]`만 넘겨 실제 인자를
  전달한 적이 없었던 것 — `build_process()`의 Linux ABI 초기
  스택을 진짜 `argc`/`argv[]`로 재구성했다(ADR-222). (2) **가장
  심각한 발견**: `servers/fs/memfs`의 응답 스크래치 버퍼
  (`g_read_scratch`)가 열린 파일 인스턴스 전체가 공유하는 하나뿐인
  슬롯이었던 것 — IPC의 "COPY" 페이지 전송(ADR-159/161)이 실제로는
  물리 프레임을 그대로 매핑하는 zero-copy라, `msh`가 `/bin/echo`를
  읽는 동안 마침 동시에 실행 중이던 기존 M32 musl fork/exec
  자기테스트가 같은 버퍼를 동시에 덮어써 실행 이미지가 손상되고
  진짜 페이지 폴트로 이어졌다 — memfs 이전엔 이 서버와 동시에
  대화하는 multi-page 소비자가 항상 하나뿐이라 절대 드러날 수
  없던 동시성 버그다. open 인스턴스별 독립 버퍼로 분리해
  해결했다(ADR-223). 부수적으로 procsrv 자신의 ELF가 다시 커져
  조용히 실패하던 M18 loader roundtrip 자기테스트도 버퍼 한도
  상향(262144→1048576)으로 함께 고쳤다. `tools/smoke-test-x86_64.sh`
  에 M52 어서션 7개 추가, 스모크(146)+SMP(11)+NUMA(24)+AVX(12)+
  net(6) 5개 회귀 스위트 전부 PASS(결과는
  [docs/done/musl-userland-porting-m52.md](docs/done/musl-userland-porting-m52.md)
  참고).
  **M53(완료)**(2026-09-11, [ADR-224](docs/design/security-model.md)
  — [ADR-171](docs/design/security-model.md)을 대체): 로그인 후 셸을
  minicore 네이티브(`userland/shell`, ADR-170) 대신 실제 포팅된
  `msh`로 완전히 교체했다 — M39가 원래 세운 목표를 완주. 착수하며
  ADR-171의 "임의 경로의 ELF를 다른 프로세스에 전달하는 일반
  메커니즘이 없다"는 전제가 이미 M32(real-libc-syscall-layer.md
  §M32)에서 무효화돼 있었다는 것을 뒤늦게 발견했다 — `procsrv`가
  컴파일 시점 데이터로 심은 ELF를 `sys_process_spawn`으로 스폰하는
  것이 정확히 그 메커니즘이었다(M32~M52 사이 아무도 이 연결을
  만들지 않았을 뿐). `servers/procsrv/main.cpp::start_session_once()`
  가 이제 고정 handle에 OP_START를 보내는 대신 `msh`를 실제로
  스폰한다 — `inherited_handles`로 procsrv 자신의 vfs 핸들+수신
  endpoint를 `msh`가 예전에 `--depends=msh:vfs,procsrv`로 받던 것과
  똑같은 순서로 주입해, `syscall_shim.c`의 고정 핸들 관례
  (`MC_VFS_HANDLE=2`/`MC_PROCSRV_HANDLE=3`)를 그대로 유지했다 — 새
  커널/IPC 기능은 필요 없었다(M18/M27이 이미 쓰던 패턴 재사용).
  `userland/shell`은 더 이상 아무도 부르지 않아 저장소에서 완전히
  제거했고(디렉터리 자체를 지웠다), `msh`도 더 이상 부팅 시점
  서비스가 아니다(`--service=msh=`/`--depends=msh:...`/
  `--linux-abi-stack=msh` 전부 제거 — procsrv의 로그인 스폰이
  유일한 진입점). `tools/smoke-test-x86_64.sh`의 `[shell] ...`
  어서션 6개 제거+`tools/smoke-test-net-x86_64.sh`의 부팅 완료
  마커를 `[msh] self-test done ok=1`로 교체, 스모크(139)+SMP(11)+
  NUMA(24)+AVX(12)+net(6) 5개 회귀 스위트 전부 PASS(결과는
  [docs/done/musl-userland-porting-m53.md](docs/done/musl-userland-porting-m53.md)
  참고).
  **M54(완료)**([ADR-225](docs/design/foundations.md) 참고): "M51+M53
  통합 검증, 새 구현 없음"이라던 계획의 예상은 틀렸다 — `msh`의
  `\|`(파이프라인)/`>`(출력 리다이렉션)를 실제로 구현하려면 새
  메커니즘이 필요했다. execve()가 fd 테이블(`g_pipe_fds[]` 등,
  `syscall_shim.c`의 로컬 BSS)을 통째로 지운다는 게 이 프로젝트에
  진짜 "fd 진실 공급원"이 없다는 것(OPEN-64)과 같은 이유로 발목을
  잡아, 새 `mc/shell_fd_binding.h`로 파이프/리다이렉션 대상 fd를
  argv의 `"@pipefd"`/`"@filefd"` 토큰으로 실어 보내고 대상 프로그램
  (`echo`/`ls`/`cat`)이 `main()` 맨 앞에서 `mc_shell_strip_bindings()`
  로 벗겨내는 관례를 새로 만들었다. 실행 중 진짜 버그 5건을
  발견·수정했다: memfs `k_max_open_files`(64→256)/`k_max_files`
  (8→16) 고정 한도 고갈, musl `open()`의 `O_CREAT` 세 번째 va_arg
  (mode) 누락, `extract_redirect`의 NUL 종료 누락, 그리고 **가장
  심각한 것**: M51이 설계한 pipesrv 참조 카운트의 "dup" 계약(fork()가
  fd 테이블을 복제하는 경우를 전제)이 msh의 실제 패턴(파이프 양끝을
  들고 있다가 각기 다른 자식에게 정확히 한 번씩 **넘겨준다**)과 안
  맞아 연쇄된 두 겹 버그다 — 1차: `SYS_fork`의 자동 dup 루프가 msh의
  매 fork()마다 참조를 불필요하게 늘려 다음 단계가 EOF를 영원히
  못 받는 무한 대기(QEMU 부트가 120초→180초→240초→300초로도 안
  끝나 처음엔 "느려졌나" 오인), 2차: 그걸 "fork() 전에 msh 자신의
  fd를 close()한다"로 "고쳤"더니 그 close()가 msh의 유일한 참조를
  자식이 넘겨받기도 전에 지워버려 파이프 자체가 사라지고 "ls | cat"
  이 조용히 exit status 1로 실패(hang이 아니라 "겉보기엔 성공"처럼
  보이는 함정이었다). 최종 해법은 `mc_shell_bind_pipe_fd`가 더 이상
  dup을 안 부르고(참조를 그대로 이어받을 뿐), 새 `mc_shell_forget_pipe_fd`
  가 서버에 알리지 않고 msh의 로컬 표만 지워 자동 dup을 막는
  "이동" 모델이다. `tools/smoke-test-x86_64.sh`에 M54 어서션 4개
  추가, 스모크+SMP+NUMA+AVX+net 5개 회귀 스위트 전부 PASS(결과는
  [docs/done/musl-userland-porting-m54.md](docs/done/musl-userland-porting-m54.md)
  참고).
  **M55(완료, 스트레치)**([ADR-226](docs/design/kernel-scheduler.md)/
  [ADR-227](docs/design/security-model.md) 참고): job control 최소
  — 착수 전 설계 검토(사용자가 "OPEN 항목을 검토해 설계 계획부터
  작성"하라고 지시)에서 계획 원문("procsrv가 `setpgid()`/`getpgid()`
  로 프로세스 그룹을 관리하고 포그라운드 그룹에 시그널을 라우팅")이
  이 프로젝트 아키텍처와 안 맞음을 발견했다 — `servers/procsrv/
  main.cpp`의 `process_entry.thread_handle`은 procsrv가 **직접**
  스폰한 프로세스(msh 자신)에만 유효하고, `mc_fork()`로 등록된
  자식(msh의 파이프라인 단계 전부)은 항상 `thread_handle=0`("모름")
  이라 procsrv는 msh의 자식에게 시그널을 보낼 방법이 원천적으로
  없다. 그래서 procsrv에 pgid 개념을 전혀 추가하지 않고 "그룹"의
  실제 주체를 msh 자신으로 옮겼다 — msh가 자기 자식의 진짜
  시그널 가능 handle(`mc_last_fork_child_thread_handle()`, M36부터
  있던 것)로 `mc_signal_send(SIGINT)`를 직접 부르고, 새 procsrv
  오퍼레이션 `MC_PROC_OP_REPORT_SIGNALED`로 그 사실만 알린다.
  커널 쪽은 `SIGINT` 하나만 핸들러 없으면 진짜로 종료하도록
  바꿨다(나머지 31개 시그널은 그대로 무시 — OPEN-75). 새 최소
  프로그램 `userland/loop-test`(`sched_yield()`를 500만 회 반복)를
  msh가 fork+exec한 뒤 인터럽트해 "자식만 종료되고 셸은 살아남는다"
  는 목표를 확인했다. 실행 중 발견: `execve()`도 syscall 리턴
  시점 시그널 확인을 거쳐, `fork()` 직후 곧바로 `SIGINT`를 보내면
  자식이 `main()`을 시작하기도 전에 죽어(`"[loop-test] starting"`
  이 로그에 전혀 없었다) "실행 중인 자식을 끊는다"는 진짜 목표를
  증명하지 못했다 — 신호를 보내기 전에 `sched_yield()`를 50회
  돌려 자식에게 실제로 스케줄될 시간을 준 것으로 고쳤다. 부수적으로
  부팅 시점에 새 VFS 파일이 하나 늘어 M54의 리다이렉션 어서션이
  고정해 둔 `open_file_id`가 36→37로 밀린 것도 겪었다.
  `tools/smoke-test-x86_64.sh`에 M55 어서션 추가, 스모크(147)+
  SMP(11)+NUMA(24)+AVX(12)+net(6) 5개 회귀 스위트 전부 PASS(결과는
  [docs/done/musl-userland-porting-m55.md](docs/done/musl-userland-porting-m55.md)
  참고).
  **M56(완료)**([ADR-228](docs/design/foundations.md)/
  [ADR-229](docs/design/kernel-ipc-objects.md) 참고, M51~M55 완료 후
  사용자 지시로 추가): "독립된 ls/cat/`[` 같은 동작이 자체 바이너리를
  갖지 않고 msh 하나의 ELF가 모두 처리하도록 바꾸자"는 지시로
  ADR-221(M52)의 "명령은 항상 별도 ELF로 fork+exec" 원칙을
  뒤집었다 — `userland/echo`/`userland/ls`/`userland/cat`을 완전히
  삭제하고 그 로직을 msh 자신의 빌트인 함수로 옮겼다. 새 빌트인
  `[`(POSIX test의 아주 좁은 부분집합)도 추가했다(사용자가 "지금
  같이 추가"를 선택). 파이프라인의 여러 빌트인이 동시에 진행돼야
  하므로("ls | cat") msh가 real musl pthread(M37,
  `pthread_create`/`pthread_join`)로 "내부적인 병렬 실행"을
  구현했다 — pthread는 handle_table/owner_space를 공유하므로
  (ADR-212) 빌트인은 msh 자신의 vfs/pipesrv handle을 그대로 쓴다.
  여러 pthread가 같은 handle_table/BSS를 공유하는 상황에서 M54의
  fd 번호 계층("지금 이 스레드의 fd 1")을 그대로 쓰면 동시에 도는
  두 빌트인이 충돌하므로, 빌트인은 그 계층을 완전히 우회해 raw
  pipe_id/{fs_handle, open_file_id}를 함수 인자로 직접 받는다.
  알려지지 않은 명령(`loop-test`뿐)은 여전히 기존 fork+exec
  경로로 떨어진다 — 셸의 일반성은 유지된다. **실행 중 이 프로젝트
  역사상 첫 진짜 커널 동시성 버그를 발견했다** — `cat` 빌트인이
  파일을 여는 건 항상 성공했는데 그 직후 읽기가 항상 0바이트를
  돌려줬다. 원인은 (1) `kern::object::handle_table`에 락이 전혀
  없어(OPEN-68이 M11부터 지적해 둔 것) `pthread_create()`가 만드는
  "새 스레드 소유 핸들"과 그 새 스레드 자신의 IPC 응답 처리가 다른
  코어에서 동시에 같은 handle_table 슬롯을 두고 경합한 것, (2) IPC
  `pages[]` 매핑 슬롯(kernel-memory.md ADR-160 슬롯 4)이 프로세스당
  고정된 자리 하나뿐이라 두 pthread가 동시에 받는 서로 다른 응답이
  같은 물리 슬롯에 겹친 것 — 둘 다였다. `handle_table`의 모든
  변경/조회 진입점을 전역 스핀락으로 감싸고, IPC 매핑 슬롯을
  스레드별 부분 슬롯(16KiB×최대 64스레드=슬롯 4의 1MiB 예산과
  정확히 일치)으로 나눠 해결했다 — OPEN-68을 해소한다.
  `tools/smoke-test-x86_64.sh`의 msh 관련 어서션을 갱신, 스모크
  (147)+SMP(11)+NUMA(24)+AVX(12)+net(6) 5개 회귀 스위트 전부
  PASS(결과는
  [docs/done/musl-userland-porting-m56.md](docs/done/musl-userland-porting-m56.md)
  참고). **musl-userland-porting.md는 이제 M51~M56 전부 완료됐다**
  — 이 계획에는 더 이상 다음 마일스톤이 없다.
- musl-userland-porting.md 완료 후, 사용자가 "부팅된 결과물을
  보여줘" → "QEMU 창을 직접 보여줘" → "VGA 카드를 실제로 표준
  텍스트 모드로 세팅하는 걸 개발해야지"로 요청을 구체화한, 어떤
  계획에도 속하지 않는 별도 확인/구현 작업을 완료했다([ADR-230](docs/design/boot-and-drivers.md)
  참고). 이 커널은 BIOS/GRUB 없이 `qboot.rom`으로 곧바로 부팅해
  VGA 카드를 실제로 텍스트 모드로 세팅해 주는 존재가 원래부터
  없었다 — M1부터 모든 검증이 디버그 시리얼 콘솔로만 이뤄져 이
  사실이 한 번도 드러난 적이 없었다. `servers/drivers/console`에
  실제 VGA BIOS의 INT 10h AH=00h AL=03h와 같은 레지스터 시퀀스
  (Sequencer/CRTC/Graphics Controller/Attribute Controller)+DAC
  팔레트 로드+VRAM 플레인 2 글꼴 비트맵 로드(`servers/login`이
  실제로 쓰는 ~20글자만, OPEN-77 신규)를 직접 구현했다. 실행 중
  발견한 진짜 버그 2건: (1) Sequencer의 Memory Mode 레지스터 변경은
  Synchronous Reset(SEQ0)으로 감싸야 문자 생성기의 실제 읽기
  경로에 반영된다는 것, (2) **가장 심각한 것**: `sys_map_phys`
  (M14, ADR-007/038/039)가 프로세스당 고정 가상주소 슬롯 하나만
  재사용한다는 것을 놓쳐, 폰트 로드용 두 번째 `sys_map_phys` 호출이
  먼저 만든 VGA 텍스트 버퍼 매핑을 조용히 다른 물리주소로 덮어써
  — 모드 세팅·DAC 팔레트·폰트 쓰기 각각은 전부 정상이었는데도 화면
  전체가 계속 검은 채로 남는 원인 파악에 오래 걸렸다 — 폰트 로드를
  먼저 끝내고 텍스트 버퍼 매핑을 마지막에 하도록 순서를 바꿔
  해결했다. QEMU `screendump`(모니터 TCP 소켓, PPM→PNG는 stdlib만
  쓴 자체 스크립트로 변환)로 "minicore login: Login successful"
  글자가 실제 육안으로 읽히는 것을 확인했고, 스모크(147)+SMP(11)+
  NUMA(24)+AVX(12)+net(6) 5개 회귀 스위트 전부 PASS(결과는
  [docs/done/console-vga-text-mode.md](docs/done/console-vga-text-mode.md)
  참고). 이 작업은 그 자체로 완결됐다 — 다음 방향(aarch64 이식 등)
  은 사용자가 정하는 대로 새 계획을 남긴다.
