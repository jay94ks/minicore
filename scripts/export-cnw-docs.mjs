#!/usr/bin/env node
// docs/ 아래에 CNW(claude-native-workflow) 문서의 읽기 전용 사본을
// 생성한다 - 정본은 항상 CNW DB이고, 여기 파일들은 GitHub에서 코드
// 없이도 설계 문서를 볼 수 있게 하기 위한 캐시일 뿐이다. 손으로
// 고치지 말고, 이 스크립트를 다시 실행해 갱신한다:
//
//   node scripts/export-cnw-docs.mjs
//
// 사전 조건: `docs` CLI가 PATH에 있고 `docs auth login`으로 로그인돼
// 있어야 한다.

import { execFileSync } from "node:child_process";
import { mkdirSync, writeFileSync, readdirSync, unlinkSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const PROJECT_ID = "cmtzsjm5c000fo401iozcc60t"; // minicore
const REPO_ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const OUT_DIR = path.join(REPO_ROOT, "docs");

const DOCS_BIN = process.platform === "win32" ? "docs.cmd" : "docs";

function docsCli(args) {
  // Windows의 npm 전역 설치 CLI는 .cmd 래퍼라 shell 없이는 실행할 수
  // 없다(EINVAL) - 인자는 전부 내부 고정값/trackingCode뿐이라 안전하다.
  const out = execFileSync(DOCS_BIN, args, { encoding: "utf-8", shell: process.platform === "win32" });
  return JSON.parse(out);
}

function frontmatter(doc) {
  return [
    "<!--",
    "  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.",
    "  정본은 claude-native-workflow(CNW)의 DB에 있습니다.",
    `  trackingCode: ${doc.trackingCode}`,
    `  status: ${doc.statusCode}`,
    `  updatedAt: ${new Date(doc.updatedAt).toISOString()}`,
    "  갱신: node scripts/export-cnw-docs.mjs",
    "-->",
    "",
  ].join("\n");
}

function main() {
  mkdirSync(OUT_DIR, { recursive: true });

  const list = docsCli(["list", PROJECT_ID]);
  const kept = new Set();
  const indexRows = [];

  for (const summary of list) {
    const doc = docsCli(["get", summary.trackingCode]);
    const filename = `${doc.trackingCode}.md`;
    kept.add(filename);
    const body = `# ${doc.title}\n\n${frontmatter(doc)}${doc.body ?? ""}`;
    writeFileSync(path.join(OUT_DIR, filename), body, "utf-8");
    indexRows.push({ trackingCode: doc.trackingCode, title: doc.title, statusCode: doc.statusCode });
  }

  // 이 프로젝트에서 지워지거나 이름이 바뀐 문서의 캐시 파일은 정리한다
  // (반대로 index.md 자체는 아래에서 다시 쓰므로 kept에 넣지 않아도 됨).
  for (const existing of readdirSync(OUT_DIR)) {
    if (existing === "index.md") continue;
    if (existing.endsWith(".md") && !kept.has(existing)) {
      unlinkSync(path.join(OUT_DIR, existing));
    }
  }

  const indexBody = [
    "# Minicore 설계 문서 (CNW 사본)",
    "",
    "<!-- 자동 생성 - node scripts/export-cnw-docs.mjs로 갱신, 손으로 편집하지 마세요. -->",
    "",
    "정본은 claude-native-workflow(CNW) 시스템 DB에 있습니다. 이 폴더는",
    "GitHub에서 코드 없이도 설계 문서를 볼 수 있게 하기 위한 읽기 전용",
    "사본(캐시)입니다.",
    "",
    "| 추적 코드 | 제목 | 상태 |",
    "|---|---|---|",
    ...indexRows
      .sort((a, b) => a.trackingCode.localeCompare(b.trackingCode))
      .map((r) => `| [${r.trackingCode}](./${r.trackingCode}.md) | ${r.title} | ${r.statusCode} |`),
    "",
  ].join("\n");
  writeFileSync(path.join(OUT_DIR, "index.md"), indexBody, "utf-8");

  console.log(`${indexRows.length}개 문서를 ${OUT_DIR}에 내보냈습니다.`);
}

main();
