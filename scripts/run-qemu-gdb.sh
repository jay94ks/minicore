#!/usr/bin/env bash
# run-qemu.sh의 디버그 변형(SP-DABFCF9F §3.1, PN-1E7798AF) - 빌드
# 절차는 동일하되, timeout 대신 -s -S로 QEMU를 첫 명령 실행 전
# 정지시켜 gdb 연결을 기다린다. PVH(-kernel 직접 부팅) 경로용 -
# GRUB/multiboot2 SMP4 재현은 run-grub-gdb.sh를 쓴다.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SMP="${MINICORE_QEMU_SMP:-1}"  # SMP 재현 시 MINICORE_QEMU_SMP=4
INITRD="${MINICORE_QEMU_INITRD:-}"  # 예: MINICORE_QEMU_INITRD=build/init.cpio

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchain-x86_64.cmake" \
    >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

KERNEL="${BUILD_DIR}/minicore.elf"

declare -a QEMU_ARGS=(-kernel "${KERNEL}")
if [[ -n "${INITRD}" ]]; then
    QEMU_ARGS+=(-initrd "${INITRD}")
fi

echo "--- QEMU가 -s -S로 일시정지 상태로 대기 중입니다 (SMP=${SMP}) ---"
echo "다른 터미널에서: gdb -x ${ROOT_DIR}/scripts/kernel.gdb"
qemu-system-x86_64 \
    "${QEMU_ARGS[@]}" \
    -serial stdio -display none -no-reboot \
    -smp "${SMP}" \
    -d cpu_reset,guest_errors -D "${BUILD_DIR}/qemu-gdb.log" \
    -s -S \
    "$@"
