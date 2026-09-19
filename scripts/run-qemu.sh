#!/usr/bin/env bash
# CMake+clang(WSL)로 커널을 빌드하고 QEMU의 -kernel 로 바로 부팅한다
# (SP-8B6B8D25 §1의 "--kernel/--initrd 직접 전달" 개발 편의 요구사항,
# DC-48565C0B에서 확정한 QEMU 부팅 스모크 테스트 자동화). GRUB/ISO를
# 거치지 않는 가장 빠른 개발 루프용이다 - WSL 안에서 실행한다.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
TIMEOUT_SECS="${MINICORE_QEMU_TIMEOUT:-5}"
# [수정, 2026-09-19, PN-9FCD59E6] run-grub.sh와 동일한 관례
# (MINICORE_QEMU_SMP=4)인데 이 스크립트만 실제로 -smp를 전달하지
# 않고 있었다 - "PVH no-initrd SMP4" 회귀 시나리오가 이 env var를
# 세팅해도 조용히 항상 SMP1로 돌고 있던 잠재 갭(디버그 로그가 따로
# 없어 아무도 눈치채지 못함).
SMP="${MINICORE_QEMU_SMP:-1}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchain-x86_64.cmake" \
    >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

KERNEL="${BUILD_DIR}/minicore.elf"

echo "--- QEMU 시리얼 출력 (최대 ${TIMEOUT_SECS}초, SMP=${SMP}) ---"
set +e
timeout "${TIMEOUT_SECS}" qemu-system-x86_64 \
    -kernel "${KERNEL}" \
    -serial stdio \
    -display none \
    -no-reboot \
    -smp "${SMP}" \
    -d cpu_reset,guest_errors \
    -D "${BUILD_DIR}/qemu.log"
status=$?
set -e

# timeout이 죽였을 때(124)는 커널이 hlt 루프에서 멈춰 있다는 뜻이라
# 정상이다(v1은 종료 장치가 없다) - 그 외 비정상 종료만 실패로 본다.
if [[ "${status}" -ne 0 && "${status}" -ne 124 ]]; then
    echo "QEMU가 비정상 종료했습니다(exit ${status}) - ${BUILD_DIR}/qemu.log 참고" >&2
    exit "${status}"
fi
