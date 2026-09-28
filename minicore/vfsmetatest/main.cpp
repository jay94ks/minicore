// minicore/vfsmetatest - SP-9039F955(VFS 파일 소유자/모드 메타데이터 +
// Chmod/Chown) E2E 검증 전용 클라이언트. setuidtest/sudotest와 동일한
// out-of-tree add_subdirectory 관례 - 부팅 매니페스트에는 포함하지
// 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
//
// livefs(/sys/live)만으로 검증 가능한 범위만 다룬다(디스크 이미지
// 불필요) - ext4/FAT류/EXEC_SETUID 경로는 이 테스트의 범위 밖(별도
// 계획으로 추적, PN 참고).
//
// 시나리오:
//  1. Stat("/sys/live/initrd.cpio") - 파일, uid=gid=0, mode=0444(§3.4).
//  2. Stat("/sys/live") - 루트 자신, 디렉터리, mode=0555.
//  3. Chmod("/sys/live/initrd.cpio", ...) - livefs는 본질적으로 읽기
//     전용이라 PermissionDenied(§4, Write와 동일 정책).
//  4. Chown("/sys/live/initrd.cpio", ...) - 이 프로세스는 root라
//     ChownHandler의 root 전용 게이트는 통과하지만, 드라이버 자신이
//     NotSupported로 거부한다(§3.4, 소유자는 항상 root 고정).
//  5. Stat("/sys/live/doesnotexist.xyz") - NotFound(회귀 없음 확인).
//
// exitCode: 0=전체 성공, 1/2/3/4/5/6=파일 Stat 단계, 7/8/9/10=루트
// Stat 단계, 11/12=Chmod 단계, 13/14=Chown 단계, 15/16=미존재 경로 Stat.
#include "libmc/process.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kInitrdPath[] = "/sys/live/initrd.cpio";
constexpr char kRootPath[] = "/sys/live";
constexpr char kMissingPath[] = "/sys/live/doesnotexist.xyz";

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

}  // namespace

extern "C" void _start() {
    // 1) 파일 Stat.
    mc::StatArgs stat1;
    stat1.path = kInitrdPath;
    stat1.pathLen = sizeof(kInitrdPath) - 1;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointStat, &stat1);
    if (token == 0 || !mc::wait(token)) {
        kFinish(1);
    }
    if (stat1.error != mc::ChannelError::None) {
        kFinish(2);
    }
    if (stat1.type != mc::FileType::Regular) {
        kFinish(3);
    }
    if (stat1.uid != 0) {
        kFinish(4);
    }
    if (stat1.gid != 0) {
        kFinish(5);
    }
    const mc::uint16_t kExpectedFileMode = mc::kPermOwnerRead | (1u << 5) | (1u << 2);  // 0444: owner/group/other read
    if (stat1.mode != kExpectedFileMode) {
        kFinish(6);
    }

    // 2) 루트 디렉터리 Stat.
    mc::StatArgs stat2;
    stat2.path = kRootPath;
    stat2.pathLen = sizeof(kRootPath) - 1;
    token = mc::submit(mc::kSyscallEndpointStat, &stat2);
    if (token == 0 || !mc::wait(token)) {
        kFinish(7);
    }
    if (stat2.error != mc::ChannelError::None) {
        kFinish(8);
    }
    if (stat2.type != mc::FileType::Directory) {
        kFinish(9);
    }
    const mc::uint16_t kExpectedDirMode =
        mc::kPermOwnerRead | mc::kPermOwnerExec | (1u << 5) | (1u << 3) | (1u << 2) | (1u << 0);  // 0555
    if (stat2.mode != kExpectedDirMode) {
        kFinish(10);
    }

    // 3) Chmod - livefs는 읽기 전용이라 PermissionDenied.
    mc::ChmodArgs chmodArgs;
    chmodArgs.path = kInitrdPath;
    chmodArgs.pathLen = sizeof(kInitrdPath) - 1;
    chmodArgs.mode = mc::kPermOwnerRead | mc::kPermOwnerWrite;
    token = mc::submit(mc::kSyscallEndpointChmod, &chmodArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(11);
    }
    if (chmodArgs.error != mc::ChannelError::PermissionDenied) {
        kFinish(12);
    }

    // 4) Chown - root 게이트는 통과하지만 드라이버가 NotSupported.
    mc::ChownArgs chownArgs;
    chownArgs.path = kInitrdPath;
    chownArgs.pathLen = sizeof(kInitrdPath) - 1;
    chownArgs.uid = 1;
    chownArgs.gid = 1;
    token = mc::submit(mc::kSyscallEndpointChown, &chownArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(13);
    }
    if (chownArgs.error != mc::ChannelError::NotSupported) {
        kFinish(14);
    }

    // 5) 미존재 경로 Stat - 회귀 없음 확인. livefs의 kLiveFsStatImpl은
    // (이 SP와 무관한 기존 동작) named/kernel/initrd.cpio 세 이름
    // 외의 경로를 NotFound가 아니라 InvalidArgument로 거부한다 -
    // 실측(2026-09-28)으로 확인한 그대로 기대값을 맞춘다.
    mc::StatArgs stat3;
    stat3.path = kMissingPath;
    stat3.pathLen = sizeof(kMissingPath) - 1;
    token = mc::submit(mc::kSyscallEndpointStat, &stat3);
    if (token == 0 || !mc::wait(token)) {
        kFinish(15);
    }
    if (stat3.error != mc::ChannelError::InvalidArgument) {
        kFinish(16);
    }

    kFinish(0);
}
