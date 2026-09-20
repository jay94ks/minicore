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
   말고 `docs cache sync cmtzsjm5c000fo401iozcc60t docs`로 다시
   생성한다(2026-09-16부로 커스텀 `scripts/export-cnw-docs.mjs` 대신
   `docs` CLI 공식 명령 사용 - 삭제된 문서의 캐시 파일 정리까지
   자동 처리, 자세한 내용은 RM-23F4B687 §6).
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
   바꾸면 그때마다 `docs cache sync cmtzsjm5c000fo401iozcc60t docs`를
   실행해 커밋한다(2026-09-14부로 "요청 시에만"에서 이 규칙으로
   대체됨; 2026-09-16부로 커스텀 node 스크립트 대신 이 공식 명령
   사용).
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
11. **계획(Plan) 목록의 항목을 처리할 땐 폐기 여부부터 조사한다**
    (설계자 지시, 2026-09-14): `docs plan list`에 있는 항목을 다룰 때는
    그 계획이 이미 폐기(다른 설계로 대체됨)됐는지 아닌지 충분히
    조사한 뒤 수행하고, 처리 후 상태를 적절히(완료/폐기 등) 바꿔
    놓는다. 설계가 나중에 뒤집혀도 예전 계획을 그냥 두지 않는다 -
    본문에 정정 섹션을 추가하거나(상태는 유지) 새 계획으로 대체
    관계를 기록한다(예: PN-05030C96 → PN-69E2D9E7).
12. **새로 정의/도입된 중요한 용어·개념은 RM-32D06563("Minicore
    용어 및 개념")에 기록한다**(설계자 지시, 2026-09-14): 이 커널
    전체에서 정의하거나 계획하거나 도입한 용어/개념/추적코드를
    모아 둔 문서 - 새 SP/PL/RM 문서가 새 용어를 도입하면 그때마다
    이 문서에도 카테고리에 맞춰 행을 추가한다.
13. **Signal 번호는 RM-B5764185("Minicore Signal 번호표")를 정본으로
    삼는다**(설계자 지시, 2026-09-14): `SignalNumber` 값은 이 표의
    번호(POSIX 표준과 동일한 0=무효/1~22/31)와 항상 일치해야 하고,
    새 신호가 필요하면 이 표에 번호부터 예약한다 - 규칙 10(syscall
    할당표)과 동일한 패턴. 기존 현황판(규칙 8/9/10/12)으로 다루기
    애매한 새 전역 현황이 또 생기면, 억지로 끼워 맞추지 말고 같은
    패턴(RM 문서 + 상호 링크 + 이 목록에 추가)으로 새 현황판을
    만든다(RM-32D06563 서두 참고).
14. **새 SP/DC 문서가 승인되면 RM-F2DAFF66("Minicore 설계공백
    검수")로 실제 코드 반영 여부를 대조한다**(설계자 지시,
    2026-09-17): 이 현황판은 "무엇을 구현했는지"가 아니라 "설계
    문서가 확정했다고 적어 둔 것 중 실제 코드에 반영되지 않은 것"을
    추적한다 - 특히 여러 필드/단계/API를 목록으로 나열하는 SP/DC
    문서의 "확정된 설계" 절에서 뒷부분 항목이 조용히 누락되는 패턴이
    실제로 발견됐다(Task::numaNode 사례). 새 SP/DC가 승인될 때마다,
    그리고 매 `/loop` 틱에 여유가 있으면 RM-F2DAFF66 §3(아직 점검
    안 한 영역)에서 하나씩 골라 점검하고 결과를 §1(발견)/§2(갭
    없음)로 옮긴다.

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
- **RM-32D06563** — Minicore 용어 및 개념(커널 전체의 용어/개념/
  추적코드 현황판) - 새 용어가 도입될 때마다 갱신(위 규칙 12).
- **RM-B5764185** — Minicore Signal 번호표(POSIX와 동일한 Signal
  번호 현황판) - 새 신호를 추가할 때마다 갱신(위 규칙 13).
- **RM-F2DAFF66** — Minicore 설계공백 검수(설계 문서가 확정한 것
  중 실제 코드에 반영 안 된 항목 추적) - 새 SP/DC 문서가 승인될
  때마다 그 "확정된 설계" 절을 실제 코드와 대조해 갱신(위 규칙
  8/9/10/12/13과 같은 패턴, 위 규칙 14).

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
  `minicore/kernel`, 유저랜드 서비스는 `minicore/net`/`tty`(devmgr/fs는
  2026-09-21부로 Process 없는 순수 커널 KernelThread로 완전 흡수돼
  `minicore/kernel` 안으로 옮겨졌다 - PN-D6A05E78).
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
