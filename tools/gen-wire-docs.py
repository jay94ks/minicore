#!/usr/bin/env python3
# ADR-195(docs/design/build-system.md) — libmc 프로토콜 헤더의
# `@wire-op` 마크업을 정규식으로 추출해 사람이 읽는 참조 표
# (docs/spec/generated/<server>-wire.md)를 생성한다. 손으로 그 결과
# 파일을 고치지 않는다 — 헤더가 바뀌면 이 스크립트를 다시 돌린다.
#
# 사용법: tools/gen-wire-docs.py <server-name> <header1.h> [header2.h ...]
#   출력: docs/spec/generated/<server-name>-wire.md
#
# 마크업 문법(ADR-195 §결정2, 표준 C 주석 한 줄 — 전처리기 확장 없음):
#   // @wire-op label=1 name=fork request=none reply="int32 child_pid"
import re
import sys
from pathlib import Path

# Windows 콘솔의 기본 코드페이지가 UTF-8이 아닐 수 있어(cp949 등),
# 한글이 섞인 print() 출력이 깨지는 것을 방지한다 — 파일 쓰기 자체는
# 항상 encoding="utf-8"을 명시하므로 이 문제와 무관하다.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")

WIRE_OP_RE = re.compile(
    r'//\s*@wire-op\s+label=(?P<label>\d+)\s+name=(?P<name>\S+)\s+'
    r'request=(?P<request>none|"[^"]*")\s+reply=(?P<reply>none|"[^"]*")'
)


def parse_header(path: Path):
    ops = []
    text = path.read_text(encoding="utf-8")
    for lineno, line in enumerate(text.splitlines(), start=1):
        m = WIRE_OP_RE.search(line)
        if not m:
            continue
        ops.append({
            "label": int(m.group("label")),
            "name": m.group("name"),
            "request": m.group("request").strip('"'),
            "reply": m.group("reply").strip('"'),
            "source": f"{path.name}:{lineno}",
        })
    return ops


def render_markdown(server: str, headers: list[Path], ops: list[dict]) -> str:
    lines = [
        f"# {server} 와이어 프로토콜 (자동 생성 — 손으로 고치지 않는다)",
        "",
        f"`tools/gen-wire-docs.py`가 {', '.join(h.name for h in headers)}의 "
        "`@wire-op` 마크업에서 추출했다(ADR-195). 헤더가 바뀌면 이 파일을 "
        "다시 생성한다 — 이 파일 자체를 손으로 고치지 않는다.",
        "",
        "| label | name | request | reply | source |",
        "|---|---|---|---|---|",
    ]
    for op in sorted(ops, key=lambda o: o["label"]):
        req = op["request"] if op["request"] != "none" else "(없음)"
        reply = op["reply"] if op["reply"] != "none" else "(없음)"
        lines.append(f"| {op['label']} | {op['name']} | {req} | {reply} | {op['source']} |")
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("사용법: tools/gen-wire-docs.py <server-name> <header1.h> [header2.h ...]",
              file=sys.stderr)
        return 1

    server = argv[1]
    headers = [Path(p) for p in argv[2:]]
    all_ops = []
    seen_labels = {}
    for h in headers:
        if not h.is_file():
            print(f"오류: 헤더를 찾을 수 없다: {h}", file=sys.stderr)
            return 1
        ops = parse_header(h)
        for op in ops:
            if op["label"] in seen_labels:
                print(f"오류: label={op['label']}이 {seen_labels[op['label']]}와 "
                      f"{op['source']}에 중복됐다", file=sys.stderr)
                return 1
            seen_labels[op["label"]] = op["source"]
        all_ops.extend(ops)

    if not all_ops:
        print(f"오류: {headers}에서 @wire-op 마크업을 하나도 찾지 못했다", file=sys.stderr)
        return 1

    repo_root = Path(__file__).resolve().parent.parent
    out_dir = repo_root / "docs" / "spec" / "generated"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{server}-wire.md"
    out_path.write_text(render_markdown(server, headers, all_ops), encoding="utf-8")
    print(f"{len(all_ops)}개 오퍼레이션을 {out_path}에 썼다")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
