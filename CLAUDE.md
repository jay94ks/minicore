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
  매크로(가칭 `MC_LAND_KERNEL`)로 커널-랜드/유저-랜드를 가르고,
  지금 `kernel/include/uapi.hpp`가 손으로 복제해 온 커널 syscall
  ABI를 `mc`의 헤더로 흡수해 `uapi.hpp`를 폐지한다
  ([ADR-200](docs/design/foundations.md), ADR-132 보강). 규칙만
  확정, 실제 실행은 [docs/plan/libs-restructure.md](docs/plan/libs-restructure.md)
  (M49 이동+리네임, M50 `uapi.hpp` 폐지+매크로 도입 — M45를
  대체) — **아직 착수 전**. [repo-layout.md](docs/design/repo-layout.md)
  의 트리는 이미 목표 상태(`libs/k/`, `libs/mc/`)로 갱신해 뒀고,
  실제 저장소는 아직 옛 경로 그대로임을 문서 상단에 명시했다.
