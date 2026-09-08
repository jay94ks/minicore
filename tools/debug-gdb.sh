#!/usr/bin/env bash
# ADR-125: MINICORE_QEMU_GDB=1로 띄워 둔 QEMU(-S -gdb tcp::1234)에
# GDB로 붙는다. 커널 ELF에서 디버그 심볼(-g, ADR-125)을 그대로 읽으므로
# 별도 symbol-file 준비가 필요 없다.
#
# 사용법 (두 터미널):
#   터미널 1: MINICORE_QEMU_GDB=1 tools/run-qemu.sh x86_64
#   터미널 2: tools/debug-gdb.sh x86_64 [빌드 디렉토리(기본: build/<arch>-clang)]
#
# 환경 변수:
#   MINICORE_GDB_BIN   gdb 실행파일 경로 (기본: PATH의 gdb)

set -euo pipefail

ARCH="${1:?"사용법: $0 <x86_64|aarch64> [빌드 디렉토리]"}"
BUILD_DIR="${2:-build/${ARCH}-clang}"
GDB_BIN="${MINICORE_GDB_BIN:-gdb}"

case "$ARCH" in
  x86_64)
    KERNEL="${BUILD_DIR}/kernel/arch/x86_64/minicore_kernel_x86_64.elf"
    ;;
  *)
    echo "사용법: $0 <x86_64|aarch64> [빌드 디렉토리]" >&2
    exit 1
    ;;
esac

if [[ ! -f "$KERNEL" ]]; then
  echo "커널 이미지가 없다: $KERNEL" >&2
  exit 1
fi

exec "$GDB_BIN" \
  -ex "set architecture i386:x86-64" \
  -ex "target remote :1234" \
  "$KERNEL"
