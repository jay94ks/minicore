#!/usr/bin/env bash
# PN-584DB994 재현율 재검증 (2026-09-20, ahci.cpp kPortTfdErr 비트
# 수정 후) - 갱신9의 표준 레시피(GRUB SMP1 + AHCI + 실제 initrd,
# 디버그 로깅 없이) 그대로 N회 반복, PANIC 문자열로 크래시 판정.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

N="${1:-40}"
export MINICORE_QEMU_SMP=1
export MINICORE_QEMU_AHCI=1
export MINICORE_QEMU_INITRD="build/initrd.cpio"

crashes=0
for i in $(seq 1 "${N}"); do
    OUT="/tmp/pn584_recheck_${i}.log"
    bash scripts/run-grub.sh >"${OUT}" 2>&1
    if grep -q "PANIC" "${OUT}"; then
        crashes=$((crashes+1))
        echo "run ${i}: CRASH"
        grep -A6 "PANIC" "${OUT}" | head -8
    fi
done

echo "===== 결과: ${crashes} / ${N} ====="
