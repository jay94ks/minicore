#!/usr/bin/env python3
"""minicore 문서/프로젝트 관리 대시보드.

docs/ 트리 뷰어, ADR·OPEN 항목 현황, plan→done 진행 상태 추적, 그리고
답변 입력 시 docs/reply.md에 기록하는 기능을 제공하는 로컬 웹 도구.

의존성 없이 표준 라이브러리만 사용한다. 실행:

    python tools/docs-dashboard/server.py [port]

기본 포트는 8765이며, 127.0.0.1에만 바인딩한다(원격 접근 불가).
"""
from __future__ import annotations

import datetime
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

ROOT = Path(__file__).resolve().parents[2]
DOCS = ROOT / "docs"
STATIC = Path(__file__).resolve().parent
REPLY_FILE = DOCS / "reply.md"

CATEGORIES = ["spec", "plan", "done", "design", "remind"]

TITLE_RE = re.compile(r"^#\s+(.+)$", re.MULTILINE)
ADR_RE = re.compile(r"^##\s+(ADR-(\d+))\.\s*(.+)$", re.MULTILINE)
OPEN_ROW_RE = re.compile(r"^~{0,2}OPEN-\d+~{0,2}$")
ANSWERED_RE = re.compile(r"^##\s+\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\s+\((OPEN-\d+)\)\s*$", re.MULTILINE)

STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/app.js": ("app.js", "application/javascript; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
}


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def first_title(path: Path) -> str:
    try:
        text = read_text(path)
    except OSError:
        return path.name
    m = TITLE_RE.search(text)
    return m.group(1).strip() if m else path.name


def build_tree() -> dict:
    tree = {}
    for cat in CATEGORIES:
        d = DOCS / cat
        entries = []
        if d.is_dir():
            for f in sorted(d.glob("*.md")):
                entries.append({
                    "name": f.name,
                    "path": f"{cat}/{f.name}",
                    "title": first_title(f),
                })
        tree[cat] = entries
    return tree


def parse_open_items() -> dict:
    open_items = {"open": [], "resolved": []}
    oi_path = DOCS / "design" / "open-items.md"
    if oi_path.exists():
        for line in read_text(oi_path).splitlines():
            line = line.strip()
            if not line.startswith("|") or not line.endswith("|"):
                continue
            cells = [c.strip() for c in line.strip("|").split("|")]
            if len(cells) < 2 or not OPEN_ROW_RE.match(cells[0]):
                continue
            resolved = cells[0].startswith("~~")
            item = {"id": cells[0].strip("~"), "cells": cells}
            (open_items["resolved"] if resolved else open_items["open"]).append(item)
    return open_items


def answered_open_ids() -> set:
    if not REPLY_FILE.exists():
        return set()
    return set(ANSWERED_RE.findall(read_text(REPLY_FILE)))


def build_adr_summary() -> dict:
    adrs = []
    design_dir = DOCS / "design"
    if design_dir.is_dir():
        for f in sorted(design_dir.glob("*.md")):
            if f.name in ("index.md", "open-items.md"):
                continue
            text = read_text(f)
            for m in ADR_RE.finditer(text):
                full, num, title = m.group(1), int(m.group(2)), m.group(3).strip()
                adrs.append({
                    "id": full,
                    "num": num,
                    "title": title,
                    "file": f"design/{f.name}",
                    "replaced": "대체" in title or "대체됨" in title,
                })
    adrs.sort(key=lambda a: a["num"])
    return {"adrs": adrs, "open_items": parse_open_items()}


def build_plan_status() -> list:
    plan_dir = DOCS / "plan"
    done_dir = DOCS / "done"
    names = set()
    if plan_dir.is_dir():
        names |= {f.name for f in plan_dir.glob("*.md")}
    if done_dir.is_dir():
        names |= {f.name for f in done_dir.glob("*.md")}
    rows = []
    for name in sorted(names):
        p, d = plan_dir / name, done_dir / name
        has_plan, has_done = p.exists(), d.exists()
        title = first_title(d if has_done else p)
        status = "완료" if has_done else "계획 중"
        rows.append({
            "name": name, "title": title,
            "plan": has_plan, "done": has_done, "status": status,
        })
    return rows


def safe_docs_path(rel: str) -> Path:
    if not rel:
        raise ValueError("path 파라미터가 필요합니다")
    resolved_docs = DOCS.resolve()
    p = (DOCS / rel).resolve()
    if resolved_docs != p and resolved_docs not in p.parents:
        raise ValueError("경로가 docs/ 밖입니다")
    if not p.is_file():
        raise FileNotFoundError(rel)
    return p


class Handler(BaseHTTPRequestHandler):
    server_version = "MinicoreDocsDashboard/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_static(self, filename: str, content_type: str):
        body = (STATIC / filename).read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        route = parsed.path
        qs = parse_qs(parsed.query)
        try:
            if route in STATIC_FILES:
                filename, content_type = STATIC_FILES[route]
                self._send_static(filename, content_type)
            elif route == "/api/tree":
                self._send_json(build_tree())
            elif route == "/api/adr":
                self._send_json(build_adr_summary())
            elif route == "/api/plan-status":
                self._send_json(build_plan_status())
            elif route == "/api/open-pending":
                answered = answered_open_ids()
                pending = [it for it in parse_open_items()["open"] if it["id"] not in answered]
                self._send_json({"items": pending})
            elif route == "/api/file":
                rel = qs.get("path", [""])[0]
                p = safe_docs_path(rel)
                self._send_json({"path": rel, "content": read_text(p)})
            elif route == "/api/reply":
                content = read_text(REPLY_FILE) if REPLY_FILE.exists() else ""
                self._send_json({"content": content})
            else:
                self._send_json({"error": "not found"}, 404)
        except FileNotFoundError:
            self._send_json({"error": "파일을 찾을 수 없습니다"}, 404)
        except ValueError as e:
            self._send_json({"error": str(e)}, 400)
        except Exception as e:  # pragma: no cover
            self._send_json({"error": str(e)}, 500)

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path != "/api/reply":
            self._send_json({"error": "not found"}, 404)
            return
        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length) if length else b""
        try:
            data = json.loads(raw.decode("utf-8")) if raw else {}
            text = (data.get("text") or "").strip()
            item_id = (data.get("id") or "").strip()
            if not text:
                raise ValueError("빈 답변은 저장할 수 없습니다")
            if item_id and not re.match(r"^OPEN-\d+$", item_id):
                raise ValueError("id는 OPEN-N 형식이어야 합니다")
            timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            heading = f"## {timestamp} ({item_id})" if item_id else f"## {timestamp}"
            is_new = not REPLY_FILE.exists()
            with REPLY_FILE.open("a", encoding="utf-8") as f:
                if is_new:
                    f.write(
                        "# 답변 기록 (미처리 답변함)\n\n"
                        "웹 대시보드(tools/docs-dashboard)의 \"답변 입력\" 탭에서 "
                        "입력한 내용을 기록한다. 각 항목은 관련 OPEN-N 추적 코드와 "
                        "함께 기록되거나, 추적 코드 없이 일반 메모로 기록된다. "
                        "docs/index.md에 등록됨.\n"
                    )
                f.write(f"\n{heading}\n\n{text}\n")
            self._send_json({"ok": True, "content": read_text(REPLY_FILE)})
        except ValueError as e:
            self._send_json({"error": str(e)}, 400)
        except Exception as e:  # pragma: no cover
            self._send_json({"error": str(e)}, 500)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"minicore 문서 대시보드 실행 중: http://127.0.0.1:{port}/  (Ctrl+C로 종료)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
