"use strict";

const CATEGORY_LABELS = {
  spec: "spec — 명세",
  plan: "plan — 실행 계획",
  done: "done — 완료 보고",
  design: "design — 설계",
  remind: "remind — 기억 사항",
};

const state = { tree: null, currentPath: null };

/* ---------- markdown rendering (dependency-free, GFM 부분집합) ---------- */

function escapeHtml(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function renderInline(text) {
  let s = escapeHtml(text);
  s = s.replace(/`([^`]+)`/g, (_, c) => `<code>${c}</code>`);
  s = s.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
  s = s.replace(/~~([^~]+)~~/g, "<del>$1</del>");
  s = s.replace(/(^|[^*])\*([^*\n]+)\*(?!\*)/g, "$1<em>$2</em>");
  s = s.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_, t, u) => `<a href="#" data-link="${u}">${t}</a>`);
  return s;
}

function isTableSep(line) {
  return /^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$/.test(line || "");
}

function splitRow(line) {
  let s = line.trim();
  if (s.startsWith("|")) s = s.slice(1);
  if (s.endsWith("|")) s = s.slice(0, -1);
  return s.split("|").map((c) => c.trim());
}

function renderMarkdown(md) {
  const lines = md.replace(/\r\n/g, "\n").split("\n");
  const out = [];
  let i = 0;

  while (i < lines.length) {
    const line = lines[i];

    if (/^```/.test(line)) {
      i++;
      const code = [];
      while (i < lines.length && !/^```/.test(lines[i])) { code.push(lines[i]); i++; }
      i++;
      out.push(`<pre><code>${escapeHtml(code.join("\n"))}</code></pre>`);
      continue;
    }

    const hm = line.match(/^(#{1,6})\s+(.*)$/);
    if (hm) {
      const level = hm[1].length;
      const text = hm[2].trim();
      const adrm = text.match(/^(ADR-\d+)\./);
      const id = adrm ? ` id="${adrm[1]}"` : "";
      out.push(`<h${level}${id}>${renderInline(text)}</h${level}>`);
      i++;
      continue;
    }

    if (line.includes("|") && isTableSep(lines[i + 1])) {
      const header = splitRow(line);
      i += 2;
      const rows = [];
      while (i < lines.length && lines[i].includes("|") && lines[i].trim() !== "") {
        rows.push(splitRow(lines[i]));
        i++;
      }
      let t = "<table><thead><tr>" + header.map((c) => `<th>${renderInline(c)}</th>`).join("") + "</tr></thead><tbody>";
      for (const r of rows) {
        const firstClean = (r[0] || "").replace(/~~/g, "").trim();
        const idAttr = /^OPEN-\d+$/.test(firstClean) ? ` id="${firstClean}"` : "";
        t += `<tr${idAttr}>` + r.map((c) => `<td>${renderInline(c)}</td>`).join("") + "</tr>";
      }
      t += "</tbody></table>";
      out.push(t);
      continue;
    }

    if (/^\s*(-{3,}|\*{3,})\s*$/.test(line)) { out.push("<hr>"); i++; continue; }

    if (/^>\s?/.test(line)) {
      const quote = [];
      while (i < lines.length && /^>\s?/.test(lines[i])) { quote.push(lines[i].replace(/^>\s?/, "")); i++; }
      out.push(`<blockquote>${renderInline(quote.join(" "))}</blockquote>`);
      continue;
    }

    if (/^\s*[-*]\s+/.test(line) || /^\s*\d+\.\s+/.test(line)) {
      const ordered = /^\s*\d+\.\s+/.test(line);
      const tag = ordered ? "ol" : "ul";
      const items = [];
      while (i < lines.length && (/^\s*[-*]\s+/.test(lines[i]) || /^\s*\d+\.\s+/.test(lines[i]))) {
        items.push(lines[i].replace(/^\s*([-*]|\d+\.)\s+/, ""));
        i++;
      }
      out.push(`<${tag}>` + items.map((it) => `<li>${renderInline(it)}</li>`).join("") + `</${tag}>`);
      continue;
    }

    if (line.trim() === "") { i++; continue; }

    const para = [line];
    i++;
    while (
      i < lines.length && lines[i].trim() !== "" &&
      !/^#{1,6}\s+/.test(lines[i]) && !/^```/.test(lines[i]) &&
      !/^\s*[-*]\s+/.test(lines[i]) && !/^\s*\d+\.\s+/.test(lines[i]) &&
      !/^>\s?/.test(lines[i]) && !(lines[i].includes("|") && isTableSep(lines[i + 1]))
    ) { para.push(lines[i]); i++; }
    out.push(`<p>${renderInline(para.join(" "))}</p>`);
  }
  return out.join("\n");
}

/* ---------- relative doc-link resolution ---------- */

function resolveDocLink(basePath, link) {
  if (/^https?:\/\//.test(link)) return { external: link };
  let [pathPart, anchor] = link.split("#");
  if (!pathPart) return { anchor };
  const baseDir = basePath.includes("/") ? basePath.slice(0, basePath.lastIndexOf("/")) : "";
  const parts = (baseDir ? baseDir.split("/") : []).concat(pathPart.split("/"));
  const stack = [];
  for (const p of parts) {
    if (p === "" || p === ".") continue;
    if (p === "..") stack.pop();
    else stack.push(p);
  }
  return { path: stack.join("/"), anchor };
}

function pathExistsInTree(path) {
  if (!state.tree) return false;
  return Object.values(state.tree).some((entries) => entries.some((e) => e.path === path));
}

/* ---------- tabs ---------- */

function activateTab(tab) {
  document.querySelectorAll(".tab-btn").forEach((b) => b.classList.toggle("active", b.dataset.tab === tab));
  document.querySelectorAll(".panel").forEach((p) => p.classList.toggle("hidden", p.id !== `panel-${tab}`));
  document.getElementById("sidebar").classList.toggle("hidden", tab !== "docs");
  if (tab === "adr") loadAdr();
  if (tab === "progress") loadProgress();
  if (tab === "reply") { loadOpenPending(); loadReplyHistory(); }
}

document.querySelectorAll(".tab-btn").forEach((b) => b.addEventListener("click", () => activateTab(b.dataset.tab)));

/* ---------- sidebar / doc viewer ---------- */

async function loadTree() {
  const res = await fetch("/api/tree");
  state.tree = await res.json();
  const sb = document.getElementById("sidebar");
  sb.innerHTML = "";
  for (const cat of Object.keys(CATEGORY_LABELS)) {
    const entries = state.tree[cat] || [];
    const h3 = document.createElement("h3");
    h3.textContent = CATEGORY_LABELS[cat];
    sb.appendChild(h3);
    const ul = document.createElement("ul");
    for (const e of entries) {
      const li = document.createElement("li");
      const a = document.createElement("a");
      a.textContent = e.title;
      a.title = e.name;
      a.dataset.path = e.path;
      a.addEventListener("click", () => loadDoc(e.path));
      li.appendChild(a);
      ul.appendChild(li);
    }
    sb.appendChild(ul);
  }
}

async function fetchDoc(path) {
  const res = await fetch(`/api/file?path=${encodeURIComponent(path)}`);
  return res.json();
}

function scrollToAnchor(container, anchor) {
  if (!anchor) return;
  const el = container.querySelector(`[id="${CSS.escape(anchor)}"]`);
  if (!el) return;
  el.scrollIntoView({ behavior: "smooth", block: "start" });
  el.classList.add("highlight-flash");
  setTimeout(() => el.classList.remove("highlight-flash"), 1400);
}

async function loadDoc(path, anchor) {
  const data = await fetchDoc(path);
  const view = document.getElementById("doc-view");
  if (data.error) {
    view.innerHTML = `<p class="hint">문서를 불러올 수 없습니다: ${escapeHtml(data.error)}</p>`;
    return;
  }
  state.currentPath = path;
  view.innerHTML = renderMarkdown(data.content);
  document.querySelectorAll(".sidebar li a").forEach((a) => a.classList.toggle("active", a.dataset.path === path));
  activateTab("docs");
  attachLinkHandlers(view, path);
  if (anchor) scrollToAnchor(view, anchor);
  else view.scrollTop = 0;
}

function attachLinkHandlers(container, basePath) {
  container.querySelectorAll("a[data-link]").forEach((a) => {
    a.addEventListener("click", (ev) => {
      ev.preventDefault();
      const resolved = resolveDocLink(basePath, a.dataset.link);
      if (resolved.external) { window.open(resolved.external, "_blank", "noopener"); return; }
      if (resolved.path && pathExistsInTree(resolved.path)) {
        loadDoc(resolved.path, resolved.anchor);
      } else if (resolved.anchor) {
        scrollToAnchor(container, resolved.anchor);
      }
    });
  });
}

/* ---------- ADR / OPEN 현황 ---------- */

async function loadAdr() {
  const res = await fetch("/api/adr");
  const data = await res.json();

  const list = document.getElementById("adr-list");
  list.innerHTML = data.adrs.map((a) => `
    <div class="adr-row${a.replaced ? " replaced" : ""}" data-file="${a.file}" data-anchor="${a.id}">
      <span class="id">${a.id}</span>
      <span class="title">${renderInline(a.title)}</span>
      <span class="file">${a.file}</span>
    </div>`).join("");
  list.querySelectorAll(".adr-row").forEach((row) => {
    row.addEventListener("click", () => loadDoc(row.dataset.file, row.dataset.anchor));
  });

  const openBody = document.getElementById("open-open");
  openBody.innerHTML = data.open_items.open.map((it) => `
    <tr>${it.cells.map((c) => `<td>${renderInline(c)}</td>`).join("")}</tr>`).join("");

  const resolvedBody = document.getElementById("open-resolved");
  resolvedBody.innerHTML = data.open_items.resolved.map((it) => `
    <tr>${it.cells.map((c) => `<td>${renderInline(c)}</td>`).join("")}</tr>`).join("");
}

/* ---------- plan → done 진행 상태 ---------- */

async function loadProgress() {
  const res = await fetch("/api/plan-status");
  const rows = await res.json();
  const body = document.getElementById("progress-body");
  body.innerHTML = rows.map((r) => `
    <tr>
      <td>${escapeHtml(r.name)}</td>
      <td>${renderInline(r.title)}</td>
      <td><span class="dot ${r.plan ? "on" : "off"}"></span></td>
      <td><span class="dot ${r.done ? "on" : "off"}"></span></td>
      <td><span class="badge ${r.done ? "done" : "pending"}">${r.status}</span></td>
    </tr>`).join("");
}

/* ---------- 답변 입력: 미결정 항목(OPEN) 별 답변 ---------- */

function linkifyAdrRefs(text, adrMap) {
  return escapeHtml(text).replace(/ADR-\d+/g, (m) => {
    const a = adrMap[m];
    return a ? `<span class="adr-ref" data-file="${a.file}" data-anchor="${a.id}">${m}</span>` : m;
  });
}

function renderDocCellLink(cellText) {
  const m = cellText.match(/\[([^\]]+)\]\(([^)]+)\)/);
  if (!m) return escapeHtml(cellText);
  return `<a href="#" data-link="${m[2]}">${escapeHtml(m[1])}</a>`;
}

async function renderDetail(path, anchor) {
  const detail = document.getElementById("open-pending-detail");
  const data = await fetchDoc(path);
  if (data.error) {
    detail.innerHTML = `<p class="hint">문서를 불러올 수 없습니다: ${escapeHtml(data.error)}</p>`;
    return;
  }
  detail.innerHTML = renderMarkdown(data.content);
  detail.querySelectorAll("a[data-link]").forEach((a) => {
    a.addEventListener("click", (ev) => {
      ev.preventDefault();
      const resolved = resolveDocLink(path, a.dataset.link);
      if (resolved.external) { window.open(resolved.external, "_blank", "noopener"); return; }
      if (resolved.path && pathExistsInTree(resolved.path)) renderDetail(resolved.path, resolved.anchor);
      else if (resolved.anchor) scrollToAnchor(detail, resolved.anchor);
    });
  });
  detail.scrollTop = 0;
  scrollToAnchor(detail, anchor);
}

async function loadOpenPending() {
  const [adrRes, pendingRes] = await Promise.all([fetch("/api/adr"), fetch("/api/open-pending")]);
  const adrData = await adrRes.json();
  const pendingData = await pendingRes.json();
  const adrMap = {};
  adrData.adrs.forEach((a) => { adrMap[a.id] = a; });

  const container = document.getElementById("open-pending-list");
  const detail = document.getElementById("open-pending-detail");
  if (!pendingData.items.length) {
    container.innerHTML = '<p class="hint">답변 대기 중인 미결정 항목이 없습니다.</p>';
    detail.innerHTML = '<p class="hint">왼쪽에서 항목을 선택하면 관련 문서가 여기 표시됩니다.</p>';
    return;
  }

  container.innerHTML = pendingData.items.map((it) => {
    const [, content = "", relatedAdr = "", docCell = ""] = it.cells;

    let detailPath = null;
    let detailAnchor = null;
    const docMatch = docCell.match(/\[([^\]]+)\]\(([^)]+)\)/);
    if (docMatch) {
      const resolved = resolveDocLink("design/open-items.md", docMatch[2]);
      detailPath = resolved.path || null;
      detailAnchor = resolved.anchor || null;
    }
    const firstAdr = relatedAdr.match(/ADR-\d+/);
    if (firstAdr && adrMap[firstAdr[0]]) {
      detailPath = adrMap[firstAdr[0]].file;
      detailAnchor = adrMap[firstAdr[0]].id;
    }
    if (!detailPath) { detailPath = "design/open-items.md"; detailAnchor = it.id; }

    return `
      <div class="pending-card" data-id="${it.id}" data-detail-path="${detailPath}" data-detail-anchor="${detailAnchor || ""}">
        <div class="pending-head">
          <span class="pending-id">${it.id}</span>
          <span class="pending-content">${escapeHtml(content)}</span>
        </div>
        <div class="pending-body">
          <div class="pending-meta">
            ${relatedAdr ? `관련 ADR: ${linkifyAdrRefs(relatedAdr, adrMap)}` : ""}
            ${docCell ? ` · 문서: ${renderDocCellLink(docCell)}` : ""}
          </div>
          <textarea rows="3" placeholder="이 항목에 대한 답변을 입력하세요..."></textarea>
          <div class="pending-actions">
            <button class="pending-save">저장</button>
            <span class="pending-status"></span>
          </div>
        </div>
      </div>`;
  }).join("");

  container.querySelectorAll(".pending-head").forEach((head) => {
    head.addEventListener("click", () => {
      const card = head.closest(".pending-card");
      const already = card.classList.contains("expanded");
      container.querySelectorAll(".pending-card.expanded").forEach((c) => c.classList.remove("expanded"));
      if (!already) {
        card.classList.add("expanded");
        renderDetail(card.dataset.detailPath, card.dataset.detailAnchor || undefined);
      } else {
        detail.innerHTML = '<p class="hint">왼쪽에서 항목을 선택하면 관련 문서가 여기 표시됩니다.</p>';
      }
    });
  });
  container.querySelectorAll(".adr-ref").forEach((el) => {
    el.addEventListener("click", (ev) => {
      ev.stopPropagation();
      renderDetail(el.dataset.file, el.dataset.anchor);
    });
  });
  container.querySelectorAll(".pending-card a[data-link]").forEach((a) => {
    a.addEventListener("click", (ev) => {
      ev.preventDefault();
      ev.stopPropagation();
      const resolved = resolveDocLink("design/open-items.md", a.dataset.link);
      if (resolved.path && pathExistsInTree(resolved.path)) renderDetail(resolved.path, resolved.anchor);
    });
  });
  container.querySelectorAll(".pending-save").forEach((btn) => {
    btn.addEventListener("click", async (ev) => {
      ev.stopPropagation();
      const card = btn.closest(".pending-card");
      const id = card.dataset.id;
      const textarea = card.querySelector("textarea");
      const status = card.querySelector(".pending-status");
      const text = textarea.value.trim();
      if (!text) { status.textContent = "빈 답변은 저장할 수 없습니다."; return; }
      status.textContent = "저장 중...";
      const res = await fetch("/api/reply", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ text, id }),
      });
      const data = await res.json();
      if (data.error) { status.textContent = `오류: ${data.error}`; return; }
      card.remove();
      document.getElementById("reply-history").innerHTML = renderMarkdown(data.content);
      if (!container.querySelector(".pending-card")) {
        container.innerHTML = '<p class="hint">답변 대기 중인 미결정 항목이 없습니다.</p>';
        detail.innerHTML = '<p class="hint">왼쪽에서 항목을 선택하면 관련 문서가 여기 표시됩니다.</p>';
      }
    });
  });
}

/* ---------- 답변 입력: 일반 메모 ---------- */

async function loadReplyHistory() {
  const res = await fetch("/api/reply");
  const data = await res.json();
  const hist = document.getElementById("reply-history");
  hist.innerHTML = data.content ? renderMarkdown(data.content) : '<p class="hint">아직 기록된 답변이 없습니다.</p>';
}

document.getElementById("reply-save").addEventListener("click", async () => {
  const input = document.getElementById("reply-input");
  const status = document.getElementById("reply-status");
  const text = input.value.trim();
  if (!text) { status.textContent = "빈 답변은 저장할 수 없습니다."; return; }
  status.textContent = "저장 중...";
  const res = await fetch("/api/reply", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text }),
  });
  const data = await res.json();
  if (data.error) { status.textContent = `오류: ${data.error}`; return; }
  status.textContent = "docs/reply.md에 저장됨";
  input.value = "";
  document.getElementById("reply-history").innerHTML = renderMarkdown(data.content);
  setTimeout(() => { status.textContent = ""; }, 3000);
});

/* ---------- init ---------- */

loadTree();
