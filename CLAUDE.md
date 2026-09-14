# CLAUDE.md

Minicore는 **하이브리드 커널 기반 범용 운영체제**를 처음부터 새로
설계·구현하는 프로젝트다(이전 시도와 목표/범위 자체가 다른 재출발).
이 프로젝트는 claude-native-workflow(CNW) 시스템으로 관리된다 —
설계/의사결정/QA 기록은 파일이 아니라 CNW의 DB에 있고, `docs` CLI 또는
MCP 도구(`cnw`)로만 읽고 쓴다. 파일을 직접 만들거나 수정해서 설계
기록을 남기지 않는다.

- CNW 프로젝트 ID: `cmtzsjm5c000fo401iozcc60t`

## 반드시 지켜야 하는 규칙

1. **추적 코드 명시**: 제안/설계 내용을 작성할 때는 관련 문서/질의의
   추적 코드(`XX-XXXXXXXX`)를 정확히 인용한다. 문서 제목만으로 가리키지
   않는다.
2. **로컬 스크래치 사본은 git 커밋 금지 (단, `docs/` 폴더는 예외)**:
   `docs new`/`docs save`에 넘기는 로컬 임시 파일은 이 저장소에 커밋하지
   않는다 — 문서의 정본은 CNW DB다. 그러나 저장소 루트의 **`docs/`
   폴더**는 GitHub에서 코드 없이도 볼 수 있게 만든 의도적인 공개용
   사본(캐시)이라 예외적으로 커밋한다. `docs/` 파일은 손으로 고치지
   말고 `node scripts/export-cnw-docs.mjs`로 다시 생성한다(자세한 내용은
   RM-23F4B687 §6).
3. **코드 관계도는 실제로 기록한다**: 구현 중 나중에 다시 파악하려면
   비용이 드는 설계 사실(예: "이 코드가 어떤 SP/DS 결정을 구현/전제
   하는지")을 `docs relation add --file <path> --line <n> --purpose
   <설명> --refs <trackingCode>`로 기록하고, 새 탐색 전엔 `docs
   relation list`로 이미 있는지 먼저 확인한다(자세한 기준은
   RM-23F4B687 §8).
4. **명시되지 않은 설계는 임의로 결정하지 않는다**: 이 문서나 SP 문서에
   없는 세부는 추측으로 채우지 말고 DC(결정 요구사항 및 요청) 문서로
   등록해 설계자의 답을 기다린다. 작업 시작 전 `docs pending
   cmtzsjm5c000fo401iozcc60t`로 답변 대기 중인 질의가 있는지 항상
   먼저 확인한다.
5. **git 발행은 반드시 CNW 파이프라인으로만, 그리고 자동으로**: 로컬에서
   직접 `git commit`/`git push`로 GitHub main에 커밋을 얹지 않는다 —
   Gitea 작업 저장소가 모르는 커밋이 생기면 다음 `docs git publish`가
   조용히 force-push해 그 커밋을 잃어버린다(2026-09-13 실제 사고,
   RM-23F4B687 §7 참고). 항상 `docs git add`→`docs git commit`→
   `docs git publish` 순서로 하고, 로컬은 그 다음 `git fetch`+
   fast-forward로만 따라간다. 이 파이프라인을 지키는 한 커밋/발행
   전에 매번 승인을 구하지 않는다(설계자 지시, 2026-09-13) — 단
   force-push/히스토리 재작성처럼 되돌리기 어려운 조작은 예외로,
   여전히 먼저 확인한다.
6. **`docs/` 캐시는 매번 갱신**: CNW 문서(SP/DC/DS/QA/RM/PL 등)를 하나라도
   바꾸면 그때마다 `scripts/export-cnw-docs.mjs`를 실행해 커밋한다
   (2026-09-14부로 "요청 시에만"에서 이 규칙으로 대체됨).
7. **체크리스트/할 일은 전부 CNW Plan(`PN-XXXXXXXX`)으로 관리한다**
   (설계자 지시, 2026-09-14): 문서 안에 "남은 것"/"이번 범위에 포함하지
   않은 것"/"아직 열려 있는 설계 영역"/"미결 사항" 같은 목록을 텍스트로만
   남기지 않는다 — 항목 하나하나를 `docs plan new <projectId> <title>
   --body <file> --refs <관련 문서 추적코드>`로 등록하고, 그 계획을 낳은
   문서 쪽 본문에도 발급받은 `PN-XXXXXXXX`를 병기해 서로 찾아갈 수 있게
   한다. **이미 끝난 작업도 사후 등록 대상이다** — 상태를 `docs plan
   status <trackingCode> completed`로 맞춘다(전체 상태 코드는 `docs plan
   statuses`). 설계자 답변을 기다리는 계획은 `in_review`로 둔다. 계획
   간 선행 조건은 `docs plan depend`. 명령/상태 흐름 전체는 Skill의
   "계획" 절 참고.
8. **새 라이브러리를 만들면 RM-7C249618("Minicore 라이브러리 목록")에
   기록한다**(설계자 지시, 2026-09-14): 커널이든 유저랜드든 새
   라이브러리 디렉터리(`minicore/libs/<name>` 또는
   `userland/{apps,libs,tests}/<name>`)를 만들 때마다 그 문서의 표에
   이름/경로/대상(커널·유저·공용)/용도를 한 행 추가한다.
9. **VFS 경로를 구현하면 RM-C65F7760("Minicore VFS 구조")에 기록한다**
   (설계자 지시, 2026-09-14): 실제 마운트 지점이든 `/sys/live/named/`
   처럼 fs 서비스 이전 단계의 임시 내부 구현이든, VFS 경로 하나가
   실제로 동작하기 시작하면 그 문서에 경로/구현 위치/상태를 기록한다.
10. **새 syscall은 RM-48E1E610("Minicore Syscall 할당표")에서 번호부터
    예약한다**(설계자 지시, 2026-09-14): 설계 제안 단계에서도(구현
    전이라도) 다음 미사용 `SyscallEndpointId` 번호로 그 표에 행을
    먼저 추가해 다른 작업과 번호가 겹치지 않게 한다 - 구현이 끝나면
    "상태" 칸만 `구현 완료`로 갱신.

## 프로젝트 핵심 문서

- **SP-8B6B8D25** — Minicore 범용 운영체제 초기 설계 명세(커널 스코프,
  부팅 조건, 디렉터리 배치, 파일시스템 구조, 가상 메모리 레이아웃).
- **RM-23F4B687** — 코딩 컨벤션(프리픽스+파스칼케이스, 네임스페이스는
  커널 앱 전용) 및 문서화 원칙.
- **QA-26450C3E** — 부팅/커널 기본 동작 QA 시나리오(현재 전부 미검증 —
  코드가 생기는 대로 실측 체크).
- **DS-D4E5C451** — 초기 설계 결정 확정본(빌드 시스템: CMake, 컴파일러:
  WSL clang, CI: 로컬 수동 검증 + QEMU 부팅 스모크 테스트 자동화, 첫
  부팅 경로: multiboot2, 부트로더는 기존 것 체인로더로 활용, 메모리
  할당자: page+buddy+NUMA, 스케줄러: 개별 큐+CPU affinity+CAS+Push/Pull
  로드밸런싱, IPC: raw binary, devmgr: 커널 서비스이나 유저랜드 동작,
  라이선스: MIT). 공개 인터페이스 registry와 procfs 스키마는 구현하며
  보강하기로 확정(=오픈 상태 유지가 결정임).
- **DC-48565C0B / DC-427BB6B2 / DC-79A2387A / DC-5AB13FFC** — 위 결정의
  근거가 된 요구분석 문서(모두 `approved`). 새로운 미결 사항이 생기면
  같은 방식(요구분석 + `docs question`)으로 등록한다.
- **RM-7C249618** — Minicore 라이브러리 목록(커널/유저랜드 전체) - 새
  라이브러리를 만들 때마다 갱신(위 규칙 8).
- **RM-C65F7760** — Minicore VFS 구조(실제로 구현된 경로만 추적) - VFS
  경로를 구현할 때마다 갱신(위 규칙 9).
- **RM-48E1E610** — Minicore Syscall 할당표(전체 `SyscallEndpointId`
  번호 현황판) - 새 syscall을 제안/구현할 때마다 갱신(위 규칙 10).

## 코딩 컨벤션 요약 (전체는 RM-23F4B687 참고)

- 언어: C++(주) + 어셈블러(하드웨어 제어/로우레벨).
- 네이밍: **자유 함수만** 프리픽스 `k`+파스칼케이스(예: `kMain`) —
  **클래스 멤버 메서드는 `k` 없이 camelCase**(예: `Serial::init()`).
  헷갈리기 쉬우니 헷갈리면 RM-23F4B687 §1을 먼저 본다.
- 네임스페이스 규칙은 **커널 앱에만** 적용 — 유저랜드 앱은 앱별 별도
  프로젝트로 간주한다.
- 헤더 가드는 전통적 `#ifndef`/`#define`(`#pragma once` 아님). 커널
  코드에서 C++ 예외 금지, `errno_t` 체계 사용. RTTI/STL은 부팅 초기엔
  freestanding 최소 범위만, 이후 단계적으로 확장.
- 디렉터리: `minicore/arch/<arch>`는 순수 부팅 stub만, 부팅 이후에도
  쓰는 아키텍처 종속 코드는 `minicore/libs/<arch_name>`, 아키텍처
  무관 early 런타임(memcpy 등)은 `minicore/libs/libkenv`, 커널은
  `minicore/kernel`, 서비스는 `minicore/devmgr`/`fs`/`net`/`tty`.
- 빌드: CMake + WSL의 clang. 첫 구현 부팅 경로는 multiboot2, 부트로더는
  기존 것을 체인로더로 활용. 라이선스: MIT.

## 기본 명령

```bash
docs auth login --api <서버 주소> --username <아이디>   # 최초 1회
docs list cmtzsjm5c000fo401iozcc60t                       # 문서 목록
docs get <trackingCode>                                   # 문서 1건 조회
docs pending cmtzsjm5c000fo401iozcc60t                    # 답변 대기 질의 목록
docs reply <questionTrackingCode> <답변 텍스트...>
docs new cmtzsjm5c000fo401iozcc60t <docTypeCode> --title <제목> --body <로컬 파일>
```

전체 명령/MCP 도구 목록과 문서 타입/상태 흐름 설명은 Skill
(`.claude/skills/claude-native-workflow/SKILL.md`)을 참고한다.
