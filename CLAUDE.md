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
- 다음 실행 대상: [docs/plan/kernel-bootstrap.md](docs/plan/kernel-bootstrap.md)
