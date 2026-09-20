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

    # [수정, 2026-09-20] pn584_alloc_connect.gdb의 STOPPED 배너 문구
    # 자체가 "DOUBLE ALLOC" 텍스트를 포함해서(정지 원인과 무관하게
    # 항상 출력됨), 예전 grep 패턴이 kPanic만 걸린 경우도 전부
    # "DOUBLE-ALLOC HIT"로 오분류했다(실측으로 발견 - 두 번의
    # "DOUBLE-ALLOC HIT" 보고가 실제로는 둘 다 kPanic이었음). 이제
    # pn584_alloc_watch.py의 실제 log() 태그(`[pn584-alloc] ***
    # DOUBLE`/`[pn584-alloc] kPanic 도달`)로 정확히 구분한다.
    if grep -qE '^\[pn584-alloc\] \*\*\* DOUBLE' "${GDB_LOG}"; then
        echo "*** DOUBLE-ALLOC/FREE HIT on attempt ${i} - see ${GDB_LOG} ***"
        exit 0
    fi
    if grep -q '^\[pn584-alloc\] kPanic 도달' "${GDB_LOG}"; then
        echo "*** kPanic HIT on attempt ${i} - see ${GDB_LOG} ***"
        grep '^\[pn584-alloc\] kPanic 도달' "${GDB_LOG}"
        exit 0
    fi
    # [수정, 갱신27] SyncCr3Watch(kSyncCr3 진입 시점 tcb->rspOld 검증)
    # 히트도 정확히 분류 - 위 두 태그와 다른 별도 문구.
    if grep -q '^\[pn584-alloc\] \*\*\* kSyncCr3' "${GDB_LOG}"; then
        echo "*** kSyncCr3 tcb corruption HIT on attempt ${i} - see ${GDB_LOG} ***"
        grep '^\[pn584-alloc\] \*\*\* kSyncCr3' "${GDB_LOG}"
        exit 0
    fi
    echo "attempt ${i}: no hit"
done

echo "=== ${MAX_TRIES} attempts done, no hit ==="
exit 1
