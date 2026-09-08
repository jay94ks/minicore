#!/usr/bin/env bash
# 빌드 산출물을 QEMU로 부팅한다.
#
# 사용법: tools/run-qemu.sh <x86_64|aarch64>
#
# TODO: 커널이 실제로 부팅 가능한 이미지를 만들면 구현한다.
#   x86_64: qemu-system-x86_64 -M q35 ...
#   aarch64: qemu-system-aarch64 -M virt ...

set -euo pipefail
echo "TODO: run-qemu.sh는 아직 구현되지 않았다 (docs/design/repo-layout.md 참고)" >&2
exit 1
