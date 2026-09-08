#!/usr/bin/env bash
# 빌드 산출물을 QEMU로 부팅한다.
#
# 사용법: tools/run-qemu.sh <x86_64|aarch64> [빌드 디렉토리(기본: build/<arch>-clang)]
#
# x86_64: QEMU에 내장된 -kernel 직접 로더는 Multiboot2도, 64비트
# ELF(EM_X86_64)도 지원하지 않는다 — 이 커널은 둘 다 해당하므로 그
# 경로로는 못 띄운다(확인 경위는 ADR-114 참고). 대신 Xen/PVH ELF Note
# 직접 부팅 경로(QEMU가 함께 배포하는 qboot.rom 펌웨어)를 쓴다. 실제
# 배포·실기 부팅 경로는 여전히 Multiboot2(ADR-017, GRUB 필요)이며 이
# 스크립트의 범위 밖이다 — 이 스크립트는 QEMU 개발 반복 전용이다.
#
# 환경 변수:
#   MINICORE_QEMU_BIN   qemu-system-<arch> 실행파일 경로 (기본: PATH 탐색)

set -euo pipefail

ARCH="${1:?"사용법: $0 <x86_64|aarch64> [빌드 디렉토리]"}"
BUILD_DIR="${2:-build/${ARCH}-clang}"

case "$ARCH" in
  x86_64)
    QEMU_BIN="${MINICORE_QEMU_BIN:-qemu-system-x86_64}"
    KERNEL="${BUILD_DIR}/kernel/arch/x86_64/minicore_kernel_x86_64.elf"

    if [[ ! -f "$KERNEL" ]]; then
      echo "커널 이미지가 없다: $KERNEL" >&2
      echo "먼저 빌드: cmake --build ${BUILD_DIR} --target minicore_kernel_x86_64" >&2
      exit 1
    fi

    exec "$QEMU_BIN" \
      -M q35 -m 256M \
      -no-reboot -no-shutdown -display none \
      -bios qboot.rom \
      -kernel "$KERNEL" \
      -chardev stdio,id=char0,mux=off -serial chardev:char0
    ;;
  aarch64)
    echo "aarch64 부트 스텁은 아직 없다 (docs/plan/kernel-bootstrap.md 범위 밖 — ADR-009 참고)" >&2
    exit 1
    ;;
  *)
    echo "사용법: $0 <x86_64|aarch64> [빌드 디렉토리]" >&2
    exit 1
    ;;
esac
