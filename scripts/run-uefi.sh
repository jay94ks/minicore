#!/usr/bin/env bash
# minicore/boot/x86_64/uefi(구 minicore/boot-uefi, PN-7FBF255A)를
# 빌드해 QEMU+OVMF(UEFI 펌웨어)로
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
# [신규, PN-7FBF255A 체크리스트 5번 - 커널 ELF 로더] efi_main이
# 이제 이 파일을 ESP 루트의 \MINICORE.ELF에서 직접 열어 읽는다 -
# bootx64.efi(PE32+)와 minicore.elf(ELF)는 링크 단계에서 전혀
# 연결된 적 없는 별개 바이너리라, GRUB/Xen이 multiboot2/PVH
# 경로에서 대신 해 주던 "커널 이미지를 읽어 들이는" 역할을 이
# UEFI 스텁이 직접 해야 한다. 기본 경로는 run-qemu.sh/run-grub.sh
# 가 이미 쓰는 커널 빌드 산출물 - 없으면(커널을 아직 안 빌드했으면)
# 경고만 찍고 UEFI 스텁 자체의 부팅/메모리맵 검증은 계속 진행한다
# (이 스크립트의 기존 용도 - 스텁 자체 검증 - 를 깨지 않기 위함).
KERNEL_ELF="${MINICORE_KERNEL_ELF:-${ROOT_DIR}/build/minicore.elf}"

cmake -S "${ROOT_DIR}/minicore/boot/x86_64/uefi" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/minicore/boot/x86_64/uefi/cmake/toolchain-uefi-x86_64.cmake" \
    >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

rm -rf "${ESP_DIR}"
mkdir -p "${ESP_DIR}/EFI/BOOT"
cp "${BUILD_DIR}/bootx64.efi" "${ESP_DIR}/EFI/BOOT/BOOTX64.EFI"

if [[ -f "${KERNEL_ELF}" ]]; then
    cp "${KERNEL_ELF}" "${ESP_DIR}/MINICORE.ELF"
else
    echo "(경고: ${KERNEL_ELF} 없음 - 커널 ELF 로더 검증은 건너뜀, 먼저 커널을 빌드하세요)"
fi

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
# [신규, 2026-09-24, PN-61D908EB 재현 중 발견] QEMU 기본 메모리(-m
# 미지정 시 128MiB)로는 커널 이미지가 커지면서(현재 imageSpan
# ~24.6MiB) UEFI 스텁의 재배치 대상 physicalBase 탐색(main.cpp
# "physicalBase search" - 1GiB 미만의 미사용 연속 영역 필요)이
# 100% 실패해(`physicalBase search NOT FOUND`) 커널 진입 자체가
# 안 되는 걸 실측 확인 - 256M으로 늘리자 즉시 성공했다. 커널이 더
# 커지면 이 값도 같이 늘려야 할 수 있다.
timeout "${TIMEOUT_SECS}" qemu-system-x86_64 \
    -machine q35 \
    -m 256M \
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

# timeout이 죽였을 때(124)는 정상이다 - 두 가지 경우 모두 여기 해당:
# (1) ExitBootServices 핸드오프 이전 실패 경로라면 OVMF가 부팅 메뉴
#     등에서 대기 중(이 스텁이 즉시 반환해 다음 부팅 옵션/셸로 넘어감),
# (2) [신규, PN-7FBF255A 체크리스트 5번] ExitBootServices가 실제로
#     성공하면 이 스텁이 그 순간부터 영원히 hlt 루프에 머무른다(더
#     이상 firmware로 돌아가면 명세 위반이라 의도적으로 안 돌아감) -
#     이 경우 시리얼 로그에 "minicore: exiting boot services"까지만
#     찍히고 그 이후 firmware BdsDxe 로그가 전혀 이어지지 않는 것으로
#     성공 여부를 구분한다(run-grub.sh의 "no-reboot" hlt 대기와 같은
#     원칙 - 다음 증분이 세그먼트 복사+GDT/CR3+커널 진입 jmp로 이
#     hlt 루프를 대체한다).
if [[ "${status}" -ne 0 && "${status}" -ne 124 ]]; then
    echo "QEMU가 비정상 종료했습니다(exit ${status})" >&2
    exit "${status}"
fi
