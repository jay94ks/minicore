#!/usr/bin/env bash
# PN-584DB994 - pn584_alloc_watch.py를 심은 채로 gdb 배치 실행을
# 반복해, 실제 크래시(kPanic) 또는 이중할당/이중반납 어느 쪽이든
# 먼저 걸리는 걸 잡는다. pn584_alloc_connect.gdb가 이제 kPanic에서도
# bt/info registers를 자동 출력하도록 갱신됨 - 이 스크립트는 그
# 출력을 남긴 로그 파일 경로를 알려주기만 한다.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

MAX_TRIES="${MAX_TRIES:-10}"
TIMEOUT_S="${TIMEOUT_S:-40}"
export MINICORE_QEMU_SMP=1
export MINICORE_QEMU_INITRD="build/initrd.cpio"
export MINICORE_QEMU_AHCI=1

for i in $(seq 1 "${MAX_TRIES}"); do
    echo "=== attempt ${i}/${MAX_TRIES} ==="
    LOG="/tmp/pn584_alloc2_qemu_${i}.log"
    bash scripts/run-grub-gdb.sh >"${LOG}" 2>&1 &
    QEMU_SHELL_PID=$!
    sleep 1.5

    GDB_LOG="/tmp/pn584_alloc2_gdb_${i}.log"
    timeout "${TIMEOUT_S}" gdb -batch -x scripts/pn584_alloc_connect.gdb >"${GDB_LOG}" 2>&1

    pkill -f "qemu-system-x86_64.*minicore-grub-gdb.iso" 2>/dev/null
    wait "${QEMU_SHELL_PID}" 2>/dev/null

    if grep -qE "DOUBLE ALLOC|DOUBLE/UNKNOWN FREE" "${GDB_LOG}"; then
        echo "*** DOUBLE-ALLOC HIT on attempt ${i} - see ${GDB_LOG} ***"
        exit 0
    fi
    if grep -q "STOPPED" "${GDB_LOG}"; then
        echo "*** kPanic HIT on attempt ${i} - see ${GDB_LOG} ***"
        exit 0
    fi
    echo "attempt ${i}: no hit"
done

echo "=== ${MAX_TRIES} attempts done, no hit ==="
exit 1
