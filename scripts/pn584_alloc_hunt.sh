#!/usr/bin/env bash
# PN-584DB994 갱신18이 제안한 조사 - PageFrameAllocator::allocOrder*/
# freeOrder 자체에 gdb 훅을 걸어 "물리 페이지 이중소유"가 반납하는
# 쪽(어떤 살아있는 소비자가 아직 쓰는 페이지를 실수로 반납)이 아니라
# 할당기 자신의 buddy 장부 버그(같은 주소를 두 번 내줌)인지를
# 직접 구별한다. pn584_hunt.sh(dr7 슬롯 워치)와 같은 오케스트레이션
# 패턴 - GRUB SMP1+AHCI+실제 initrd를 -s -S로 띄우고 gdb를 붙여
# scripts/pn584_alloc_watch.py를 심는다. 크래시(PANIC)를 기다리는 게
# 아니라 "이중할당/이중반납 자체가 한 번이라도 관측되는가"가 질문이라,
# 매 시도가 정상 종료(타임아웃)해도 실패가 아니다 - DOUBLE 로그 유무만
# 본다.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

MAX_TRIES="${MAX_TRIES:-15}"
TIMEOUT_S="${TIMEOUT_S:-20}"
export MINICORE_QEMU_SMP=1
export MINICORE_QEMU_INITRD="build/initrd.cpio"
export MINICORE_QEMU_AHCI=1

for i in $(seq 1 "${MAX_TRIES}"); do
    echo "=== attempt ${i}/${MAX_TRIES} ==="
    LOG="/tmp/pn584_alloc_qemu_${i}.log"
    bash scripts/run-grub-gdb.sh >"${LOG}" 2>&1 &
    QEMU_SHELL_PID=$!
    sleep 1.5

    GDB_LOG="/tmp/pn584_alloc_gdb_${i}.log"
    timeout "${TIMEOUT_S}" gdb -batch -x scripts/pn584_alloc_connect.gdb >"${GDB_LOG}" 2>&1

    pkill -f "qemu-system-x86_64.*minicore-grub-gdb.iso" 2>/dev/null
    wait "${QEMU_SHELL_PID}" 2>/dev/null

    if grep -qE "DOUBLE ALLOC|DOUBLE/UNKNOWN FREE" "${GDB_LOG}"; then
        echo "*** HIT on attempt ${i} - see ${GDB_LOG} ***"
        exit 0
    fi
    CALLS=$(grep -c "^\[pn584-alloc\]" "${GDB_LOG}" 2>/dev/null || echo 0)
    echo "attempt ${i}: no double-alloc/double-free observed (log lines: ${CALLS})"
done

echo "=== ${MAX_TRIES} attempts done, no double-alloc/double-free caught ==="
exit 1
