#!/usr/bin/env bash
# 유저랜드(minicore/init 등)를 빌드해 CPIO(newc) 아카이브로 묶는다 -
# QEMU/GRUB에 module2로 실어 실제 유저 프로세스가 뜨는 시나리오
# (SP-68182FBD/PN-D3C05C0B)를 재현하기 위한 개발 도구. **[제외,
# 2026-09-20, SP-43331889/QU-23B339AB/QU-ECEE5990/QU-5FC58B06]
# devmgr/fs 둘 다 더 이상 이 initrd에 담기지 않는다** - Process 없는
# 순수 커널 KernelThread로 완전 흡수돼 커널 자신(minicore.elf)에
# 직접 링크된다(kmain.cpp의 kSpawnDevmgrKernelThread()/
# kSpawnFsKernelThread() 참고).
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
# 한다(경로 접두어 없이 "init"/"pubreg") - cpio 아카이브 안에도 이
# 이름 그대로 들어가도록 스테이징 디렉터리 안에서 상대경로로 넘긴다.
# pubreg(PN-185406F6, 2026-09-17) 추가 - init과 동일한 이유.
# authmgr(PN-24A2B6F5/PN-CFEAEF40, 2026-09-20) 추가 - v1은 스캐폴딩만
# (실제 프로토콜은 후속 세션), 부팅 매니페스트 왕복 자체는 pubreg와
# 동일하게 검증 대상.
# dbgtarget/proctest(PN-012D6310, 2026-09-22) 추가 - 이 둘은
# kSpawnServiceProcesses 고정 스폰 목록에는 없다(각자 main.cpp 상단
# 주석 참고 - 반드시 실제 SpawnProcess syscall로만 스폰돼야 함) - 그냥
# initrd.cpio 안에 원본 ELF 바이트로만 실려, 유저랜드 스포너가
# `/sys/live/initrd.cpio`를 직접 읽어(libcpio) 그 바이트를
# SpawnProcess의 imageBuffer로 넘기는 용도다.
# dbgdriver(PN-0556C759, 2026-09-26) 추가 - dbgtarget을 실제로
# SpawnProcess하는 쪽(위와 동일한 이유로 원본 ELF 바이트만 필요)이면서
# 동시에, dbgtarget/proctest와 달리 kmain.cpp의 TEMP 스폰 경로로
# 이름("dbgdriver")이 매치돼 부팅 시 자동 실행되는 대상이기도 하다
# (minicore/dbgdriver/main.cpp 상단 주석 참고) - 이 재현 조사 기간
# 동안만 필요, 조사가 끝나면 이 줄들도 TEMP 스폰 경로와 함께 되돌린다.
cp "${BUILD_USERLAND_DIR}/minicore-init/init" "${STAGE_DIR}/init"
cp "${BUILD_USERLAND_DIR}/minicore-pubreg/pubreg" "${STAGE_DIR}/pubreg"
cp "${BUILD_USERLAND_DIR}/minicore-authmgr/authmgr" "${STAGE_DIR}/authmgr"
cp "${BUILD_USERLAND_DIR}/minicore-dbgtarget/dbgtarget" "${STAGE_DIR}/dbgtarget"
cp "${BUILD_USERLAND_DIR}/minicore-proctest/proctest" "${STAGE_DIR}/proctest"
cp "${BUILD_USERLAND_DIR}/minicore-dbgdriver/dbgdriver" "${STAGE_DIR}/dbgdriver"
# socktest/sockclient(PN-CC0F4EAC, 2026-09-27) 추가 - dbgdriver와
# 동일한 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 둘 다 독립
# 프로세스로 자동 실행된다(SpawnProcess/CreateThread 둘 다 시도했다가
# PN-395F4D89를 발견해 이 방식으로 우회, socktest/main.cpp 상단
# 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-socktest/socktest" "${STAGE_DIR}/socktest"
cp "${BUILD_USERLAND_DIR}/minicore-sockclient/sockclient" "${STAGE_DIR}/sockclient"
# sockinherit/sockinheritchild(PN-CC0F4EAC 항목7, 2026-09-27) 추가 -
# dbgdriver/dbgtarget과 동일한 이유 - sockinherit이 kmain.cpp의 TEMP
# 스폰 경로로 자동 실행되며 sockinheritchild를 진짜 SpawnProcess(fd
# 상속 플래그)로 직접 스폰한다(sockinherit/main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-sockinherit/sockinherit" "${STAGE_DIR}/sockinherit"
cp "${BUILD_USERLAND_DIR}/minicore-sockinheritchild/sockinheritchild" "${STAGE_DIR}/sockinheritchild"
# epolltest(SP-6350DEBB/PN-7562DA62, 2026-09-27) 추가 - sockclient를
# 그대로 재사용하는 짝(epolltest/main.cpp 상단 주석 참고). **이
# initrd로 socktest를 함께 켤 땐 반드시 socktest의 TEMP 훅을 빼거나
# epolltest의 TEMP 훅을 빼서, 둘 다 "socktest.sock" 리스너를 동시에
# bind하지 않게 한다.**
cp "${BUILD_USERLAND_DIR}/minicore-epolltest/epolltest" "${STAGE_DIR}/epolltest"
# authtest(PN-24A2B6F5, 2026-09-27) 추가 - dbgdriver와 동일한 이유로
# kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다(main.cpp 상단
# 주석 참고). 조사가 끝나면 이 줄도 TEMP 스폰 경로와 함께 되돌린다.
cp "${BUILD_USERLAND_DIR}/minicore-authtest/authtest" "${STAGE_DIR}/authtest"
# setuidtest(DC-90A66932 (A), 2026-09-28) 추가 - authtest와 동일한
# 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다(main.cpp
# 상단 주석 참고). 검증이 끝나면 이 줄도 TEMP 스폰 경로와 함께 되돌린다.
cp "${BUILD_USERLAND_DIR}/minicore-setuidtest/setuidtest" "${STAGE_DIR}/setuidtest"
# sudotest(DC-34764C25 항목1, 2026-09-28) 추가 - setuidtest와 동일한
# 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다(main.cpp
# 상단 주석 참고). 검증이 끝나면 이 줄도 TEMP 스폰 경로와 함께 되돌린다.
cp "${BUILD_USERLAND_DIR}/minicore-sudotest/sudotest" "${STAGE_DIR}/sudotest"
# vfsmetatest(SP-9039F955, 2026-09-28) 추가 - setuidtest/sudotest와
# 동일한 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다
# (main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-vfsmetatest/vfsmetatest" "${STAGE_DIR}/vfsmetatest"
# createusertest(DC-CC83F7BE, 2026-09-28) 추가 - 위 테스트들과 동일한
# 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다(main.cpp
# 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-createusertest/createusertest" "${STAGE_DIR}/createusertest"
# lrutest(PN-B6DB692C, 2026-09-28) 추가 - 위 테스트들과 동일한 이유로
# kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다(main.cpp 상단
# 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-lrutest/lrutest" "${STAGE_DIR}/lrutest"
# exittest/exitwaiter(PN-5EDE3C96 항목1, 2026-09-29) 추가 - sockinherit/
# sockinheritchild와 동일한 이유(exitwaiter가 진짜 SpawnProcess로
# exittest를 직접 스폰) - kmain.cpp의 TEMP 스폰 경로는 exitwaiter만
# 이름으로 매치해 부팅 시 자동 실행된다(main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-exittest/exittest" "${STAGE_DIR}/exittest"
cp "${BUILD_USERLAND_DIR}/minicore-exitwaiter/exitwaiter" "${STAGE_DIR}/exitwaiter"
# timerfdtest(PN-96265AE4/PN-0F56DE4B, 2026-09-29) 추가 - 위 테스트들과
# 동일한 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다
# (main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-timerfdtest/timerfdtest" "${STAGE_DIR}/timerfdtest"
# granttest(DC-2B22FBF0, 2026-09-29) 추가 - 위 테스트들과 동일한
# 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다(main.cpp
# 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-granttest/granttest" "${STAGE_DIR}/granttest"
# diskmetatest(PN-2A0981B7 항목3, 2026-09-29) 추가 - 위 테스트들과 동일한
# 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다(main.cpp
# 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-diskmetatest/diskmetatest" "${STAGE_DIR}/diskmetatest"
# fatdisktest(PN-2A0981B7 항목3, 2026-09-29) 추가 - diskmetatest와
# 동일한 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다
# (main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-fatdisktest/fatdisktest" "${STAGE_DIR}/fatdisktest"
# setuidspawntest(PN-2A0981B7 항목4, 2026-09-29) 추가 - exitwaiter와
# 동일한 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다
# (main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-setuidspawntest/setuidspawntest" "${STAGE_DIR}/setuidspawntest"
# exfatdisktest(PN-2A0981B7 항목1, 2026-09-29) 추가 - 위 테스트들과
# 동일한 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시 자동 실행된다
# (main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-exfatdisktest/exfatdisktest" "${STAGE_DIR}/exfatdisktest"
# ntfsdisktest(PN-2A0981B7 항목2, QU-9F8AD7A8 답변(A), 2026-09-29) 추가 -
# 위 테스트들과 동일한 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시
# 자동 실행된다(main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-ntfsdisktest/ntfsdisktest" "${STAGE_DIR}/ntfsdisktest"
# ext4rwtest(PN-2A0981B7 항목5, QU-1E7DFB8C 답변(A), 2026-09-29) 추가 -
# 위 테스트들과 동일한 이유로 kmain.cpp의 TEMP 스폰 경로로 부팅 시
# 자동 실행된다(main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-ext4rwtest/ext4rwtest" "${STAGE_DIR}/ext4rwtest"
# signalmasktarget/signalmasktest(PN-A1A0B595 선행 작업, QU-28D7C7B1
# 답변(A), 2026-09-29) 추가 - 위 테스트들과 동일한 이유로 정상적인
# init 대체(사이드카 cpio)로 부팅 시 실행된다(main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-signalmasktarget/signalmasktarget" "${STAGE_DIR}/signalmasktarget"
cp "${BUILD_USERLAND_DIR}/minicore-signalmasktest/signalmasktest" "${STAGE_DIR}/signalmasktest"
# signalfdchild/signalfdtest(PN-A1A0B595, 2026-09-29) 추가 - 위
# 테스트들과 동일한 이유로 정상적인 init 대체(사이드카 cpio)로 부팅
# 시 실행된다(main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-signalfdchild/signalfdchild" "${STAGE_DIR}/signalfdchild"
cp "${BUILD_USERLAND_DIR}/minicore-signalfdtest/signalfdtest" "${STAGE_DIR}/signalfdtest"
# rtsignalchild/rtsignaltest(PN-FD706AF6, 2026-09-29) 추가 - 위
# 테스트들과 동일한 이유로 정상적인 init 대체(사이드카 cpio)로 부팅
# 시 실행된다(main.cpp 상단 주석 참고).
cp "${BUILD_USERLAND_DIR}/minicore-rtsignalchild/rtsignalchild" "${STAGE_DIR}/rtsignalchild"
cp "${BUILD_USERLAND_DIR}/minicore-rtsignaltest/rtsignaltest" "${STAGE_DIR}/rtsignaltest"

mkdir -p "$(dirname "${OUT_PATH}")"
(
    cd "${STAGE_DIR}"
    printf 'init\npubreg\nauthmgr\ndbgtarget\nproctest\ndbgdriver\nsocktest\nsockclient\nsockinherit\nsockinheritchild\nepolltest\nauthtest\nsetuidtest\nsudotest\nvfsmetatest\ncreateusertest\nlrutest\nexittest\nexitwaiter\ntimerfdtest\ngranttest\ndiskmetatest\nfatdisktest\nsetuidspawntest\nexfatdisktest\nntfsdisktest\next4rwtest\nsignalmasktarget\nsignalmasktest\nsignalfdchild\nsignalfdtest\nrtsignalchild\nrtsignaltest\n' | cpio -o -H newc --quiet > "${OUT_PATH}"
)

echo "initrd 생성 완료: ${OUT_PATH} ($(stat -c%s "${OUT_PATH}" 2>/dev/null || stat -f%z "${OUT_PATH}") bytes, init+pubreg+authmgr+dbgtarget+proctest+dbgdriver+socktest+sockclient+sockinherit+sockinheritchild+epolltest+authtest+setuidtest+sudotest+vfsmetatest+createusertest+lrutest+exittest+exitwaiter+timerfdtest+granttest+diskmetatest+fatdisktest+setuidspawntest+exfatdisktest+ntfsdisktest+ext4rwtest+signalmasktarget+signalmasktest+signalfdchild+signalfdtest+rtsignalchild+rtsignaltest)"
