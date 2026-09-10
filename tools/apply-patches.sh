#!/usr/bin/env bash
# third_party/<project> submodule 체크아웃을 <output-dir>로 복사하고
# 그 복사본 위에 third_party/patches/<project>/*.patch를 이름순으로
# 적용한다(ADR-022) — 실제 submodule 체크아웃 자체는 절대 건드리지
# 않는다(항상 pristine 유지, real-libc-syscall-layer.md §M28이 요구한
# "build/_patched/<project>류 작업 트리" 방식).
#
# 사용법: tools/apply-patches.sh <project> <output-dir>
#
# CMake에서 add_custom_command로 호출한다(예: libc/CMakeLists.txt가
# ${CMAKE_BINARY_DIR}/_patched/musl로 이 스크립트를 호출) — 매번
# <output-dir>를 완전히 새로 만든다(증분 아님, musl 서브모듈이
# 충분히 작아 매번 복사해도 비용이 낮다, YAGNI).

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "사용법: tools/apply-patches.sh <project> <output-dir>" >&2
  exit 1
fi

PROJECT="$1"
OUT_DIR="$2"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/third_party/$PROJECT"
PATCH_DIR="$REPO_ROOT/third_party/patches/$PROJECT"

if [[ ! -d "$SRC_DIR" ]]; then
  echo "오류: third_party/$PROJECT 체크아웃을 찾을 수 없다: $SRC_DIR" >&2
  exit 1
fi

# submodule이 clean한지 확인한다 — 더러운 상태(사람이 손으로 고친
# 흔적)에 패치를 적용하면 어느 변경이 패치 탓이고 어느 게 사람 탓인지
# 구분할 수 없어진다.
if git -C "$SRC_DIR" diff --quiet -- . && git -C "$SRC_DIR" diff --cached --quiet -- .; then
  :
else
  echo "오류: third_party/$PROJECT가 clean하지 않다(로컬 수정 있음) — 먼저 되돌린다" >&2
  git -C "$SRC_DIR" status --short >&2
  exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$(dirname "$OUT_DIR")"
cp -r "$SRC_DIR" "$OUT_DIR"

# 복사본에 submodule의 `.git` 파일(상위 저장소의 .git/modules를
# 가리키는 gitlink)이 그대로 따라온다 — 복사된 위치에서는 그 경로가
# 깨져 있어 git apply가 "not a git repository"로 실패한다. 이 복사본은
# 애초에 git 저장소가 아니어야 한다(순수 파일 트리) — 지운다.
rm -rf "$OUT_DIR/.git"

if [[ -d "$PATCH_DIR" ]]; then
  shopt -s nullglob
  patches=("$PATCH_DIR"/*.patch)
  shopt -u nullglob
  for p in "${patches[@]}"; do
    echo "적용 중: $(basename "$p")"
    # git apply가 아니라 표준 patch(1)를 쓴다 — 이 저장소의
    # core.autocrlf=true 설정 때문에 git apply가 CRLF 원본
    # 파일(musl은 CRLF로 체크아웃돼 있다)에 대해 EOL을 자기 마음대로
    # 정규화하며 "Skipped patch"로 조용히 아무것도 안 하는 것을
    # 실제로 겪었다(2026-09-10) — patch(1)는 그런 정규화를 하지
    # 않는다. 패치 파일 자체는 순수 LF로 생성해 둔다(줄바꿈 방식은
    # C 컴파일러에 영향이 없다).
    if ! patch -p1 --directory="$OUT_DIR" < "$p"; then
      echo "오류: $(basename "$p") 적용 실패" >&2
      exit 1
    fi
  done
fi

echo "완료: $OUT_DIR"
