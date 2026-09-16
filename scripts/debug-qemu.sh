#!/usr/bin/env bash
# QEMU gdb stub 기반 커널 디버깅 워크플로(PN-1E7798AF) - PN-584DB994/
# PN-57CF48DB/PN-6049A353 세 조사가 전부 "정적 추론 한계, 실제
# 단일 스텝 디버깅 필요"라는 같은 결론에 막혀 등록됐다(2026-09-16
# 문서/계획 리뷰 세션 관찰). run-qemu.sh/run-grub.sh와 같은 빌드
# 로직을 재사용하되, `-s -S`(TCP 1234에 gdb stub 열고 첫 명령 전에
# 정지)로 실행한다 - 이 스크립트 자신은 QEMU만 띄우고 즉시 반환한다
# (백그라운드 실행 없음 - 호출부가 &로 백그라운드에 돌리고 별도
# 터미널/명령으로 scripts/kernel.gdb를 붙여야 한다).
#
# 사용법:
#   scripts/debug-qemu.sh <시나리오> [추가 QEMU 인자...]
#   시나리오: pvh | pvh-initrd | grub-smp4 | synth
#
# 다른 터미널(또는 백그라운드 실행 뒤)에서:
#   gdb -x scripts/kernel.gdb
# 이 자동으로 심볼 로드 + target remote :1234 + kPanic 브레이크포인트
# 까지 걸어 준다 - 그 뒤 `continue`로 실행을 재개한다.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SCENARIO="${1:-pvh}"
shift || true

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchain-x86_64.cmake" \
    >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

KERNEL="${BUILD_DIR}/minicore.elf"

# 시나리오별 QEMU 인자 - 4개 표준 회귀 시나리오(CLAUDE.md 검증 관례)와
# 그대로 대응시킨다. GRUB SMP4는 이미 만들어진 ISO(run-grub.sh 계열이
# grub-iso-smp4/를 준비해 둔 상태)가 있다고 가정 - 없으면 이 스크립트
# 호출 전에 먼저 grub-mkrescue로 만들어 둔다.
declare -a QEMU_ARGS
case "${SCENARIO}" in
    pvh)
        QEMU_ARGS=(-kernel "${KERNEL}")
        ;;
    pvh-initrd)
        QEMU_ARGS=(-kernel "${KERNEL}" -initrd "${BUILD_DIR}/init.cpio")
        ;;
    grub-smp4)
        ISO="${BUILD_DIR}/minicore-grub-smp4.iso"
        if [[ ! -f "${ISO}" ]]; then
            echo "minicore-grub-smp4.iso가 없습니다 - 먼저 grub-iso-smp4/를 준비하고 grub-mkrescue로 만드세요" >&2
            exit 1
        fi
        QEMU_ARGS=(-cdrom "${ISO}" -smp 4)
        ;;
    synth)
        QEMU_ARGS=(-kernel "${KERNEL}" -initrd /tmp/synth_initrd.cpio)
        ;;
    *)
        echo "알 수 없는 시나리오: ${SCENARIO} (pvh|pvh-initrd|grub-smp4|synth 중 하나)" >&2
        exit 1
        ;;
esac

echo "--- QEMU gdb stub 시작 (시나리오: ${SCENARIO}) ---"
echo "다른 터미널에서: gdb -x ${ROOT_DIR}/scripts/kernel.gdb"
echo "(TCP 1234에서 gdb 연결을 기다리며 정지한 상태로 시작합니다 - continue 전까지 아무 코드도 실행되지 않습니다)"

exec qemu-system-x86_64 \
    "${QEMU_ARGS[@]}" \
    -serial stdio \
    -display none \
    -no-reboot \
    -s -S \
    "$@"
