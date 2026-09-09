#!/usr/bin/env bash
# tools/make-fs-test-images.sh — docs/plan/system-servers-bringup.md
# §M16 검증용 FAT32/ext4 이미지를 만든다. 각 이미지의 루트에
# hello.txt 하나(알려진 내용)를 심어 둔다 — servers/fs/fat32·
# servers/fs/ext4가 마운트 후 이 파일을 열어 읽어서 그 결과를 확인한다
# (virtio-blk의 "쓰고 다시 읽어 일치 확인"과 같은 정신, 다만 여기는
# 호스트가 미리 쓴 내용을 게스트가 읽기만 한다 — 두 FS 모두 v1은
# 읽기전용, ADR-057/129).
#
# 필요한 호스트 도구: mkfs.vfat + mcopy(dosfstools/mtools, msys2
# pacman: `pacman -S dosfstools mingw-w64-x86_64-mtools`), mke2fs
# (실제 e2fsprogs — Android SDK platform-tools에 딸려 오는 것으로
# 충분하다, 커널 소스가 아니라 온디스크 포맷만 만들 뿐이라 버전
# 민감도가 낮다). mke2fs의 `-d`(디렉터리로 채우기) 옵션은 이 환경의
# 빌드에서 깨져 있어(경로 인코딩 버그, 직접 확인함) 대신
# tools/mkfs-ext4-testfile.py가 빈 이미지에 파일 하나를 직접 써
# 넣는다.
#
# 사용법: tools/make-fs-test-images.sh <fat32 출력경로> <ext4 출력경로>
set -euo pipefail

FAT32_OUT="${1:?"사용법: $0 <fat32 출력경로> <ext4 출력경로>"}"
EXT4_OUT="${2:?"사용법: $0 <fat32 출력경로> <ext4 출력경로>"}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

FAT32_CONTENT="hello fat32 world"
EXT4_CONTENT="hello ext4 world"

if [[ ! -f "$FAT32_OUT" ]]; then
  TMP_DIR="$(mktemp -d)"
  trap 'rm -rf "$TMP_DIR"' EXIT
  printf '%s\n' "$FAT32_CONTENT" > "$TMP_DIR/hello.txt"
  dd if=/dev/zero of="$FAT32_OUT" bs=1M count=4 status=none
  mkfs.vfat -F 32 -n TESTFAT "$FAT32_OUT" > /dev/null
  mcopy -i "$FAT32_OUT" "$TMP_DIR/hello.txt" ::hello.txt
  echo "만듦: $FAT32_OUT (hello.txt=\"$FAT32_CONTENT\")"
fi

if [[ ! -f "$EXT4_OUT" ]]; then
  mke2fs -t ext4 -b 1024 -O ^has_journal,^64bit,^metadata_csum,^uninit_bg \
    -F -q "$EXT4_OUT" 4096
  TMP_FILE="$(mktemp)"
  printf '%s\n' "$EXT4_CONTENT" > "$TMP_FILE"
  python3 "$SCRIPT_DIR/mkfs-ext4-testfile.py" "$EXT4_OUT" hello.txt "$TMP_FILE"
  rm -f "$TMP_FILE"
  echo "만듦: $EXT4_OUT (hello.txt=\"$EXT4_CONTENT\")"
fi
