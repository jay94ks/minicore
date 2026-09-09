#!/usr/bin/env bash
# x86_64 SMP 스모크 테스트: docs/plan/smp-fpu-bringup.md §M10(ADR-055)의
# AP 기동+IPI TLB shootdown을 QEMU -smp N으로 검증한다.
#
# 기본 스모크 테스트(smoke-test-x86_64.sh)와 분리하는 이유:
# MINICORE_QEMU_SMP는 opt-in이라(ADR-125의 "기본값 유지" 패턴) 기존
# 1코어 경로는 이 스크립트 없이도 항상 그대로 통과해야 한다 — SMP
# 검증은 여기서만 다룬다.
#
# 사용법: tools/smoke-test-smp-x86_64.sh [빌드 디렉토리(기본: build/x86_64-clang)]
#
# 환경 변수:
#   MINICORE_QEMU_BIN   qemu-system-x86_64 실행파일 경로(run-qemu.sh로 그대로 전달)
#   MINICORE_QEMU_SMP   코어 수(기본: 4) — run-qemu.sh에 그대로 전달한다.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-build/x86_64-clang}"
TIMEOUT_SEC=40  # M12부터 initrun.elf가 눈에 띄게 커져(cpio/INI 파서 추가) 20s가 종종 부족했다.
export MINICORE_QEMU_SMP="${MINICORE_QEMU_SMP:-4}"

declare -a EXPECTED=(
  "hello from kernel"
  "[smp] BSP apic_id=0"
  "[acpi] madt_ok=1 cpu_count=${MINICORE_QEMU_SMP}"
  "[smp] AP apic_id=1 online cpu_index=1"
  "[smp] AP apic_id=2 online cpu_index=2"
  "[smp] AP apic_id=3 online cpu_index=3"
  "[smp] bring_up_aps done online_count=${MINICORE_QEMU_SMP}"
  "[smp] online_cpu_count=${MINICORE_QEMU_SMP}"
  # M1~M9 데모(kernel_main.cpp의 demo_page_table 등)가 map_page/
  # unmap_page/protect_page를 부르는 순간마다 이제 실제로 3개 AP에
  # IPI shootdown을 브로드캐스트하고 응답을 기다린다 — 그 경로가
  # 죽지 않고 이후 데모 전체(M1~M9 검증 문자열)가 그대로 끝까지
  # 진행됨을 아래에서 함께 확인한다.
  "[initrun] kernel received boot call ok=1 label=0xb007 (expect 0xb007) - 부팅 성공"
  "[fpu] thread A done all_preserved=1"
  "[fpu] thread B done all_preserved=1"
)

LOG_FILE="$(mktemp)"
trap 'rm -f "$LOG_FILE"' EXIT

timeout "$TIMEOUT_SEC" "$SCRIPT_DIR/run-qemu.sh" x86_64 "$BUILD_DIR" \
  > "$LOG_FILE" 2>&1

failed=0
for expected in "${EXPECTED[@]}"; do
  if grep -qF "$expected" "$LOG_FILE"; then
    echo "PASS: \"$expected\" 확인"
  else
    echo "FAIL: \"$expected\"를 찾지 못했다" >&2
    failed=1
  fi
done

if [[ "$failed" -ne 0 ]]; then
  echo "--- 전체 로그 ---" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

exit 0
