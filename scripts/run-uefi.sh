#!/usr/bin/env bash
# minicore/boot-uefi(PN-7FBF255A)를 빌드해 QEMU+OVMF(UEFI 펌웨어)로
# 부팅한다 - run-qemu.sh(PVH)/run-grub.sh(BIOS/GRUB multiboot2)와는
# 완전히 다른 세 번째 부팅 경로다. UEFI는 El Torito(GRUB ISO)가 아니라
# FAT 파일시스템의 \EFI\BOOT\BOOTX64.EFI 관례로 부팅하므로, QEMU의
# "-drive file=fat:rw:<dir>,format=raw" 즉석 FAT 합성 기능을 그대로
# 쓴다(mtools/mkfs.vfat 설치 불필요 - PN-7FBF255A "체크리스트 1번
# 실측 검증" 절에서 실측 확인).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-uefi"
ESP_DIR="${BUILD_DIR}/esp"
OVMF_CODE="${MINICORE_OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS_SRC="${MINICORE_OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
OVMF_VARS_RW="${BUILD_DIR}/OVMF_VARS_4M.rw.fd"
TIMEOUT_SECS="${MINICORE_QEMU_TIMEOUT:-8}"

cmake -S "${ROOT_DIR}/minicore/boot-uefi" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/minicore/boot-uefi/cmake/toolchain-uefi-x86_64.cmake" \
    >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

rm -rf "${ESP_DIR}"
mkdir -p "${ESP_DIR}/EFI/BOOT"
cp "${BUILD_DIR}/bootx64.efi" "${ESP_DIR}/EFI/BOOT/BOOTX64.EFI"

# OVMF_VARS는 QEMU가 실행 중 써야 해서 읽기전용 시스템 사본을 매번
# 새로 복사한다(원본 오염 방지).
cp "${OVMF_VARS_SRC}" "${OVMF_VARS_RW}"

echo "--- QEMU(OVMF UEFI) 부팅, ESP=${ESP_DIR} (최대 ${TIMEOUT_SECS}초) ---"
set +e
# [실측 확인] 이 OVMF 빌드(Ubuntu 패키지 기본 OVMF_CODE_4M.fd)는
# BdsDxe 진행 로그를 debugcon이 아니라 COM1 시리얼로 보낸다 -
# -debugcon으로는 빈 로그만 남았고, -serial file:<path>로 정확히
# "BdsDxe: loading/starting Boot0001 ..." 시퀀스를 확인했다.
timeout "${TIMEOUT_SECS}" qemu-system-x86_64 \
    -machine q35 \
    -drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
    -drive if=pflash,format=raw,file="${OVMF_VARS_RW}" \
    -drive file=fat:rw:"${ESP_DIR}",format=raw \
    -display none \
    -no-reboot \
    -serial file:"${BUILD_DIR}/ovmf-serial.log"
status=$?
set -e

echo "--- OVMF 시리얼 로그 (${BUILD_DIR}/ovmf-serial.log) ---"
cat "${BUILD_DIR}/ovmf-serial.log" 2>/dev/null | tr -d '\r' | grep -a 'BdsDxe:' || echo "(BdsDxe 로그 없음 - 부팅 실패 가능성)"

# timeout이 죽였을 때(124)는 OVMF가 부팅 메뉴 등에서 대기 중이라는
# 뜻이라 정상이다(이 스텁은 즉시 반환하므로 다음 부팅 옵션이나 셸로
# 넘어가 계속 대기하는 게 예상 동작 - run-grub.sh의 "no-reboot" hlt
# 대기와 같은 원칙).
if [[ "${status}" -ne 0 && "${status}" -ne 124 ]]; then
    echo "QEMU가 비정상 종료했습니다(exit ${status})" >&2
    exit "${status}"
fi
