#!/usr/bin/env bash
# run-grub.sh의 디버그 변형(SP-DABFCF9F §3.1, PN-1E7798AF) - ISO
# 빌드 절차는 동일하되, timeout 대신 -s -S로 QEMU를 첫 명령 실행 전
# 정지시켜 gdb 연결을 기다린다. GRUB multiboot2 경로 + SMP 재현
# (PN-584DB994의 SMP4 시나리오 등)용 - PVH 경로는 run-qemu-gdb.sh를
# 쓴다.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
ISO_ROOT="${BUILD_DIR}/grub-iso-gdb"
ISO_PATH="${BUILD_DIR}/minicore-grub-gdb.iso"
SMP="${MINICORE_QEMU_SMP:-1}"  # SMP4 재현 시 MINICORE_QEMU_SMP=4
INITRD="${MINICORE_QEMU_INITRD:-}"  # 예: MINICORE_QEMU_INITRD=build/init.cpio(호스트 경로)

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchain-x86_64.cmake" \
    >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

rm -rf "${ISO_ROOT}"
mkdir -p "${ISO_ROOT}/boot/grub"
cp "${BUILD_DIR}/minicore.elf" "${ISO_ROOT}/boot/minicore.elf"
{
    echo 'set timeout=0'
    echo 'set default=0'
    echo 'menuentry "minicore" {'
    if [[ -n "${INITRD}" ]]; then
        cp "${INITRD}" "${ISO_ROOT}/boot/init.cpio"
        echo '    multiboot2 /boot/minicore.elf'
        echo '    module2 /boot/init.cpio init.cpio'
    else
        echo '    multiboot2 /boot/minicore.elf'
    fi
    echo '    boot'
    echo '}'
} > "${ISO_ROOT}/boot/grub/grub.cfg"

grub-mkrescue -o "${ISO_PATH}" "${ISO_ROOT}" >/dev/null 2>&1

echo "--- QEMU(-cdrom GRUB ISO)가 -s -S로 일시정지 상태로 대기 중입니다 (SMP=${SMP}) ---"
echo "다른 터미널에서: gdb -x ${ROOT_DIR}/scripts/kernel.gdb"
qemu-system-x86_64 \
    -cdrom "${ISO_PATH}" \
    -serial stdio -display none -no-reboot \
    -smp "${SMP}" \
    -d cpu_reset,guest_errors -D "${BUILD_DIR}/qemu-grub-gdb.log" \
    -s -S \
    "$@"
