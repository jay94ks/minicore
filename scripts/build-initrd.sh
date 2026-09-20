#!/usr/bin/env bash
# 유저랜드(minicore/init 등)를 빌드해 CPIO(newc) 아카이브로 묶는다 -
# QEMU/GRUB에 module2로 실어 실제 유저 프로세스가 뜨는 시나리오
# (SP-68182FBD/PN-D3C05C0B)를 재현하기 위한 개발 도구. **[제외,
# 2026-09-20, SP-43331889/QU-23B339AB/QU-ECEE5990] devmgr은 더 이상
# 이 initrd에 담기지 않는다** - Process 없는 순수 커널 KernelThread로
# 완전 흡수돼 커널 자신(minicore.elf)에 직접 링크된다(kmain.cpp의
# kSpawnDevmgrKernelThread() 참고).
#
# 이 프로젝트에 지금까지 이 도구가 없어서, ResourceGroup(PN-4190BBD3)/
# ChunkedList 버그(PN-A8D235E7)/idle 스택 안전성(PN-2008220B)/
# 하드웨어 브레이크포인트(PN-87D6B615) 등 여러 세션이 "initrd가 없어
# 이 환경에서 실측 재현 못함"을 반복해서 남겨야 했다 - 이 스크립트가
# 그 공백을 메운다. libcpio(minicore/libs/libcpio/cpio.cpp)가 요구하는
# 정확한 포맷(newc, "070701" 매직, 파일명에 경로 접두어 없이 정확히
# "init")을 GNU cpio -H newc로 그대로 만족한다(실측 확인 - 이 파일
# 작성 시 xxd/python으로 헤더 바이트까지 직접 검증했다).
#
# 사용법:
#   scripts/build-initrd.sh [출력 경로, 기본값 build/initrd.cpio]
#
# 그 뒤 run-grub.sh/run-grub-gdb.sh에 MINICORE_QEMU_INITRD=<출력 경로>
# 로 넘기면 GRUB module2로 실어 부팅한다.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_USERLAND_DIR="${ROOT_DIR}/build-userland"
STAGE_DIR="${ROOT_DIR}/build/initrd-stage"
OUT_PATH="${1:-${ROOT_DIR}/build/initrd.cpio}"

cmake -S "${ROOT_DIR}/userland" -B "${BUILD_USERLAND_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/userland/cmake/toolchain-userland-x86_64.cmake" \
    >/dev/null
cmake --build "${BUILD_USERLAND_DIR}" >/dev/null

rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"
# 파일명은 kmain.cpp의 kLogCpioEntry가 정확히 매치하는 이름 그대로여야
# 한다(경로 접두어 없이 "init"/"pubreg"/"fs") - cpio 아카이브 안에도
# 이 이름 그대로 들어가도록 스테이징 디렉터리 안에서 상대경로로
# 넘긴다. pubreg(PN-185406F6, 2026-09-17)/fs(PN-452FF696, 2026-09-17)
# 추가 - init과 동일한 이유.
cp "${BUILD_USERLAND_DIR}/minicore-init/init" "${STAGE_DIR}/init"
cp "${BUILD_USERLAND_DIR}/minicore-pubreg/pubreg" "${STAGE_DIR}/pubreg"
cp "${BUILD_USERLAND_DIR}/minicore-fs/fs" "${STAGE_DIR}/fs"

mkdir -p "$(dirname "${OUT_PATH}")"
(
    cd "${STAGE_DIR}"
    printf 'init\npubreg\nfs\n' | cpio -o -H newc --quiet > "${OUT_PATH}"
)

echo "initrd 생성 완료: ${OUT_PATH} ($(stat -c%s "${OUT_PATH}" 2>/dev/null || stat -f%z "${OUT_PATH}") bytes, init+pubreg+fs)"
