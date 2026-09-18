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
TIMEOUT_SECS="${MINICORE_QEMU_TIMEOUT:-10}"

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

# [실측 확인, 2026-09-18] 이 스크립트를 곧바로 연달아 재실행하면
# (이전 QEMU가 timeout으로 막 SIGTERM 종료된 직후) 가끔 OVMF가
# BdsDxe 로그조차 못 찍고 조용히 멈춘 채 timeout까지 가는 현상을
# 재현했다(펌웨어 로직 문제가 아니라 - 재시도하면 정상 재현되고, 이후
# 정상 부팅 시 메모리맵 파싱 결과까지 전부 올바르게 나옴을 확인) -
# 직전 QEMU 프로세스가 /mnt/c(NTFS 백엔드) 위의 pflash 파일 핸들을
# 완전히 놓기 전에 다음 실행의 cp/열기가 겹치는 것으로 추정된다.
# sleep 1로는 4회 중 1회 여전히 재현됐고, sleep 2로 늘리자 4회 연속
# 무결 - 완전히 없앴다는 보장은 아니지만(타이밍 의존이라 0%로
# 단언 불가) 반복 재현 빈도를 크게 낮췄다.
sleep 2

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
