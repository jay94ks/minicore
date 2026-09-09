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
  (M21 선점형 스케줄링 ~ M26 실제 libc 포팅 재도전)를 새로 계획했다.
  **M21~M23 완료**. M21: LAPIC 타이머 기반 선점(BSP·ring3 한정,
  ADR-176)과 그 과정에서 발견한 TSS.RSP0 전역 공유 버그 수정
  (ADR-177), 결과는
  [docs/done/general-purpose-completion-m21.md](docs/done/general-purpose-completion-m21.md).
  M22: `sys_process_kill`(ADR-178)+procsrv의 wait/kill 자기테스트,
  결과는
  [docs/done/general-purpose-completion-m22.md](docs/done/general-purpose-completion-m22.md).
  M23: `sys_fork`가 handle_table 전체를 복제(ADR-179)+procsrv의
  fork+exec fd 상속 자기테스트, 결과는
  [docs/done/general-purpose-completion-m23.md](docs/done/general-purpose-completion-m23.md).
  다음 실행 대상은 M24(유저랜드 동적 메모리). **주의**: `servers/*`/`userland/*` 코드를
  고친 뒤에는 일반 `cmake --build`만으로는 `bootdisk.img`가
  갱신되지 않는다(add_custom_target이라 기본 빌드에 안 걸림) —
  `--target minicore_bootdisk_image`를 반드시 추가로 돌려야 한다
  (M22에서 실제로 겪음, docs/done/general-purpose-completion-m22.md
  참고).
