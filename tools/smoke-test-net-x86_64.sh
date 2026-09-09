#!/usr/bin/env bash
# x86_64 네트워킹 스모크 테스트: docs/plan/general-purpose-completion.md
# §M25의 virtio-net 드라이버+netsrv DHCP 왕복을 QEMU
# -netdev user(SLIRP)로 검증한다.
#
# 기본 스모크 테스트(smoke-test-x86_64.sh)와 분리하는 이유:
# MINICORE_QEMU_NET은 opt-in이라(ADR-125의 "기본값 유지" 패턴) 기존
# 경로는 이 스크립트 없이도 항상 그대로 통과해야 한다 — 네트워킹
# 검증은 여기서만 다룬다. 나머지(부트 디스크/xHCI/테스트 디스크/
# FAT32·ext4 이미지)는 smoke-test-x86_64.sh와 완전히 같은 전체
# 시스템이 필요하다(virtio-net/netsrv도 그 부트 디스크의 서비스
# 목록에 포함돼 있다, servers/CMakeLists.txt) — 그래서 같은 기본값
# 설정을 그대로 반복한다.
#
# 사용법: tools/smoke-test-net-x86_64.sh [빌드 디렉토리(기본: build/x86_64-clang)]
#
# 환경 변수:
#   MINICORE_QEMU_BIN   qemu-system-x86_64 실행파일 경로(run-qemu.sh로 그대로 전달)
#   그 외(BOOTDISK/XHCI/TESTDISK/FAT32DISK/EXT4DISK)는 smoke-test-x86_64.sh
#   와 동일한 기본값 규칙.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-build/x86_64-clang}"
TIMEOUT_SEC=120  # smoke-test-x86_64.sh와 같은 전체 시스템 부팅이 먼저 필요하다.
export MINICORE_QEMU_NET=1

if [[ -z "${MINICORE_QEMU_BOOTDISK:-}" ]]; then
  DEFAULT_BOOTDISK="${BUILD_DIR}/servers/bootdisk.img"
  if [[ -f "$DEFAULT_BOOTDISK" ]]; then
    export MINICORE_QEMU_BOOTDISK="$DEFAULT_BOOTDISK"
  fi
fi

if [[ -z "${MINICORE_QEMU_XHCI:-}" ]]; then
  export MINICORE_QEMU_XHCI=1
fi

if [[ -z "${MINICORE_QEMU_TESTDISK:-}" ]]; then
  export MINICORE_QEMU_TESTDISK="${BUILD_DIR}/testdisk.img"
fi

if [[ -z "${MINICORE_QEMU_FAT32DISK:-}" ]]; then
  export MINICORE_QEMU_FAT32DISK="${BUILD_DIR}/fat32-test.img"
fi
if [[ -z "${MINICORE_QEMU_EXT4DISK:-}" ]]; then
  export MINICORE_QEMU_EXT4DISK="${BUILD_DIR}/ext4-test.img"
fi
if [[ ! -f "$MINICORE_QEMU_FAT32DISK" || ! -f "$MINICORE_QEMU_EXT4DISK" ]]; then
  bash "$SCRIPT_DIR/make-fs-test-images.sh" "$MINICORE_QEMU_FAT32DISK" "$MINICORE_QEMU_EXT4DISK"
fi

declare -a EXPECTED=(
  "hello from kernel"
  "[virtio-net] init ok"
  "[netsrv] got mac from virtio-net"
  "[netsrv] dhcp discover sent=1"
  "[netsrv] udp roundtrip ok=1"
  # 네트워킹 왕복 뒤로도 나머지 시스템(M12~M24)이 그대로 끝까지
  # 진행됨을 함께 확인한다 — virtio-net/netsrv가 이 자리(virtio-blk
  # 다음, fat32/ext4 이전)에 새로 끼어들어도 나머지 부팅 순서를
  # 깨지 않았는가가 이 스위트의 회귀 검증 대상이다.
  "[shell] self-test done"
)

LOG_FILE="$(mktemp)"
trap 'rm -f "$LOG_FILE"' EXIT

timeout "$TIMEOUT_SEC" "$SCRIPT_DIR/run-qemu.sh" x86_64 "$BUILD_DIR" \
  > "$LOG_FILE" 2>&1

failed=0
for expected in "${EXPECTED[@]}"; do
  if grep -qF "$expected" "$LOG_FILE"; then
    echo "PASS: \"$expected\" 확인"
  else
    echo "FAIL: \"$expected\"를 찾지 못했다" >&2
    failed=1
  fi
done

if [[ "$failed" -ne 0 ]]; then
  echo "--- 전체 로그 ---" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

exit 0
