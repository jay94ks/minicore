#!/usr/bin/env bash
# PN-584DB994 gdb watchpoint hunt orchestration (2026-09-20 tick, TEMP -
# not part of the repro recipe docs, delete once the hunt concludes).
# Repeats: launch GRUB SMP1 + MINICORE_QEMU_AHCI=1 + real initrd under
# QEMU -s -S, attach gdb with scripts/pn584_connect.gdb (arms the
# per-call dr7-slot watchpoint from scripts/pn584_watch.py), let it run
# up to TIMEOUT_S, then tear down and try again up to MAX_TRIES times or
# until a hit is found.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

MAX_TRIES="${MAX_TRIES:-15}"
TIMEOUT_S="${TIMEOUT_S:-45}"
export MINICORE_QEMU_SMP=1
export MINICORE_QEMU_INITRD="build/initrd.cpio"
export MINICORE_QEMU_AHCI=1

for i in $(seq 1 "${MAX_TRIES}"); do
    echo "=== attempt ${i}/${MAX_TRIES} ==="
    LOG="/tmp/pn584_qemu_${i}.log"
    bash scripts/run-grub-gdb.sh >"${LOG}" 2>&1 &
    QEMU_SHELL_PID=$!
    # give QEMU a moment to bind the gdbstub port before gdb connects.
    sleep 1.5

    GDB_LOG="/tmp/pn584_gdb_${i}.log"
    timeout "${TIMEOUT_S}" gdb -batch -x scripts/pn584_connect.gdb >"${GDB_LOG}" 2>&1
    GDB_RC=$?

    # tear down this attempt's QEMU (pkill by port owner is more robust
    # than tracking the exact qemu-system-x86_64 pid through the wrapper
    # script's subshell).
    pkill -f "qemu-system-x86_64.*minicore-grub-gdb.iso" 2>/dev/null
    wait "${QEMU_SHELL_PID}" 2>/dev/null

    if grep -q "EXTERNAL WRITE" "${GDB_LOG}"; then
        echo "*** HIT on attempt ${i} - see ${GDB_LOG} ***"
        exit 0
    fi
    if grep -q "Breakpoint.*kPanic" "${GDB_LOG}"; then
        echo "*** kPanic reached without a caught external write on attempt ${i} - see ${GDB_LOG} (calls armed: $(grep -c 'external write' -i "${GDB_LOG}")) ***"
    fi
    CALLS=$(grep -o "calls.*" "${GDB_LOG}" | tail -1)
    echo "gdb rc=${GDB_RC}, no hit this attempt"
done

echo "=== ${MAX_TRIES} attempts done, no external write caught ==="
exit 1
