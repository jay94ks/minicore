#!/usr/bin/env bash
# x86_64 lazy FPU/AVX 스모크 테스트: docs/plan/smp-fpu-bringup.md
# §M11b(ADR-133) 검증목표(b) — "-cpu 옵션으로 AVX가 있는/없는 두 QEMU
# 구성 모두에서 정상 동작함을 확인한다"를 그대로 두 번 부팅해 확인한다.
#
# 기본 스모크 테스트(smoke-test-x86_64.sh)가 이미 "AVX 없음"(QEMU 기본
# CPU 모델) 경로를 매번 확인하고 있어, 여기서는 그 사실 + "AVX 있음"
# (-cpu max) 경로까지 한 스크립트에서 함께 재확인한다.
#
# 사용법: tools/smoke-test-avx-x86_64.sh [빌드 디렉토리(기본: build/x86_64-clang)]
#
# 환경 변수:
#   MINICORE_QEMU_BIN   qemu-system-x86_64 실행파일 경로(run-qemu.sh로 그대로 전달)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-build/x86_64-clang}"
TIMEOUT_SEC=25  # M12부터 initrun.elf가 커지는 추세라 여유를 좀 더 둔다.

declare -a COMMON_EXPECTED=(
  "hello from kernel"
  "[fpu] thread A done all_preserved=1"
  "[fpu] thread B done all_preserved=1"
  "[fpu-lazy] thread C done all_preserved=1"
  "[initrun] kernel received boot call ok=1 label=0xb007 (expect 0xb007) - 부팅 성공"
)

failed=0

run_one() {
  local label="$1"
  local cpu_value="$2"
  shift 2
  local -a extra_expected=("$@")

  local log_file
  log_file="$(mktemp)"

  if [[ -n "$cpu_value" ]]; then
    MINICORE_QEMU_CPU="$cpu_value" timeout "$TIMEOUT_SEC" "$SCRIPT_DIR/run-qemu.sh" x86_64 "$BUILD_DIR" \
      > "$log_file" 2>&1
  else
    timeout "$TIMEOUT_SEC" "$SCRIPT_DIR/run-qemu.sh" x86_64 "$BUILD_DIR" \
      > "$log_file" 2>&1
  fi

  local -a all_expected=("${COMMON_EXPECTED[@]}" "${extra_expected[@]}")
  local one_failed=0
  for expected in "${all_expected[@]}"; do
    if grep -qF "$expected" "$log_file"; then
      echo "PASS[$label]: \"$expected\" 확인"
    else
      echo "FAIL[$label]: \"$expected\"를 찾지 못했다" >&2
      one_failed=1
    fi
  done

  if [[ "$one_failed" -ne 0 ]]; then
    echo "--- [$label] 전체 로그 ---" >&2
    cat "$log_file" >&2
    failed=1
  fi
  rm -f "$log_file"
}

# 구성 1: QEMU 기본 CPU 모델 — XSAVE/AVX 없음(FXSAVE 폴백 경로).
run_one "no-avx" "" \
  "[fpu] xsave_avail=0 avx_avail=0 using_xsave=0 area_size=512"

# 구성 2: -cpu max — XSAVE+AVX 있음.
run_one "avx" "max" \
  "[fpu] xsave_avail=1 avx_avail=1 using_xsave=1"

exit "$failed"
