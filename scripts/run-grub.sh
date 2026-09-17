#!/usr/bin/env bash
# CMake+clang(WSL)로 커널을 빌드하고, grub-mkrescue로 GRUB ISO를 만들어
# QEMU -cdrom 으로 부팅한다(PL-FC38956C - multiboot2 경로 검증용).
# run-qemu.sh(PVH 전용, QEMU -kernel 직접 전달)와는 별도 스크립트로
# 분리했다 - 이쪽은 실제 GRUB 부트로더를 거치는 더 느리지만 더 현실적인
# 경로다. grub-mkrescue/xorriso가 설치돼 있어야 한다(WSL에
# grub-pc-bin/grub-efi-amd64-bin/xorriso 패키지로 설치 확인됨).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
ISO_ROOT="${BUILD_DIR}/grub-iso"
ISO_PATH="${BUILD_DIR}/minicore-grub.iso"
TIMEOUT_SECS="${MINICORE_QEMU_TIMEOUT:-8}"
SMP="${MINICORE_QEMU_SMP:-1}"  # SMP4 재현 시 MINICORE_QEMU_SMP=4(run-grub-gdb.sh와 동일 관례)
# [신규, 2026-09-17] run-grub-gdb.sh의 MINICORE_QEMU_INITRD와 동일한
# 관례 - 실제 initrd(init/devmgr 등이 든 CPIO newc 아카이브, 호스트
# 경로)를 GRUB module2로 함께 실어 부팅한다. 미지정 시 기존 동작
# 그대로(initrd 없음, "modules=0") - 이 스크립트를 쓰던 기존 호출부
# 전부 그대로 호환.
INITRD="${MINICORE_QEMU_INITRD:-}"

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
        cp "${INITRD}" "${ISO_ROOT}/boot/initrd.cpio"
        echo '    multiboot2 /boot/minicore.elf'
        echo '    module2 /boot/initrd.cpio initrd.cpio'
    else
        echo '    multiboot2 /boot/minicore.elf'
    fi
    echo '    boot'
    echo '}'
} > "${ISO_ROOT}/boot/grub/grub.cfg"

grub-mkrescue -o "${ISO_PATH}" "${ISO_ROOT}" >/dev/null 2>&1

echo "--- QEMU(-cdrom GRUB ISO) 시리얼 출력 (최대 ${TIMEOUT_SECS}초, SMP=${SMP}) ---"
set +e
timeout "${TIMEOUT_SECS}" qemu-system-x86_64 \
    -cdrom "${ISO_PATH}" \
    -serial stdio \
    -display none \
    -no-reboot \
    -smp "${SMP}" \
    -d cpu_reset,guest_errors \
    -D "${BUILD_DIR}/qemu-grub.log"
status=$?
set -e

# timeout이 죽였을 때(124)는 커널이 hlt 루프에서 멈춰 있다는 뜻이라
# 정상이다(v1은 종료 장치가 없다) - 그 외 비정상 종료만 실패로 본다.
if [[ "${status}" -ne 0 && "${status}" -ne 124 ]]; then
    echo "QEMU가 비정상 종료했습니다(exit ${status}) - ${BUILD_DIR}/qemu-grub.log 참고" >&2
    exit "${status}"
fi
