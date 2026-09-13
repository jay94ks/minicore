# CLAUDE.md

이 프로젝트는 claude-native-workflow 시스템으로 관리된다. 문서/설계
기록은 파일이 아니라 이 시스템의 DB에 있고, `docs` CLI 또는 MCP 도구로만
읽고 쓴다 - 파일을 직접 만들거나 수정해서 설계 기록을 남기지 않는다.

## 반드시 지켜야 하는 세 가지 규칙

1. **추적 코드 명시**: 어떤 제안을 하거나 설계 내용을 작성할 때는 관련된
   문서/질의의 추적 코드(`XX-XXXXXXXX` 형식)를 정확히 인용한다. 예:
   "이 결정은 DC-A1B2C3D4에서..." 문서 제목만으로 가리키지 않는다.
2. **로컬 스크래치 사본은 git 커밋 금지**: `docs new`/`docs save`
   등에 넘기는 로컬 임시 파일은 정상적인 작업 방식이지만(지금 편집 중인
   문서를 로컬에 복사해 Edit 도구로 다듬은 뒤 다시 올리는 것), 이 사본을
   이 프로젝트의 git 저장소에 `git add`/`git commit`하지 않는다 - 문서의
   정본은 시스템 DB이고, 편집용 임시 파일이 소스 트리에 섞여 들어가면
   안 된다. 임시 디렉터리 등 git 추적 대상 밖에 둔다.
3. **코드 관계도는 실제로 기록한다**: 여러 파일을 가로지르는 탐색이라
   다시 파악하려면 비용이 드는 발견을 했으면(예: "이 버그가 세 파일에
   걸쳐 있다는 걸 확인했다") `docs relation add`로 기록하고, 새 탐색을
   시작하기 전엔 `docs relation list --q <키워드>`로 이미 기록된 게
   있는지 먼저 확인한다 - 사소한 한 줄짜리 조회까지 전부 남기는 감사
   로그는 아니다(정확한 기준은 Skill의 "코드 관계도" 절 참고). 현재
   체크아웃된 브랜치가 자동 감지되므로 별도 설정이 필요 없다.

## 기본 명령

```bash
docs auth login --api <서버 주소> --username <아이디>   # 최초 1회
docs list <projectId>                                    # 문서 목록
docs get <trackingCode>                                   # 문서 1건 조회
docs new <projectId> <docTypeCode> --title <제목> --body <로컬 파일>
docs pending <projectId>                                  # 답변 대기 질의 목록
docs reply <questionTrackingCode> <답변 텍스트...>
```

전체 명령/MCP 도구 목록과 문서 타입/상태 흐름 설명은 Skill
(`.claude/skills/claude-native-workflow/SKILL.md`)을 참고한다.
