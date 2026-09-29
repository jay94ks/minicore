// minicore/ntfsdisktest - PN-2A0981B7 항목2(QU-9F8AD7A8 답변(A), NTFS
// 실제 Chmod 쓰기 경로) E2E 검증 전용 클라이언트. 실제 mkntfs 이미지
// (ntfs-3g로 NTFSTEST.TXT를 채움, 초기 $STANDARD_INFORMATION은
// READONLY 비트 없음)가 fs 커널 서비스의 TEMP kTryAutoMountBlockDevice()
// 확장(exfatdisktest와 동일한 이유로 이번 세션이 --rw-mount cmdline +
// TEMP NTFS 마운트 훅으로 /sys/mnt에 마운트 - 검증 후 원복)에 의해
// 마운트된 상태를 전제로 한다 - MINICORE_QEMU_AHCI=1 +
// MINICORE_QEMU_AHCI_DISK=<이미지 경로>로 부팅해야 한다.
//
// 시나리오:
//  1. Stat - 초기 mode=0644(쓰기 가능, READONLY 비트 없음), uid=gid=0
//     (mountUid 반사), size=23.
//  2. Chmod(owner-write 제거) -> None 기대(실제 온디스크 쓰기 성공 -
//     $STANDARD_INFORMATION.fileAttributes에 READONLY 비트 추가 +
//     fixup 재계산).
//  3. 재-Stat -> mode=0444로 바뀌었는지 확인(Chmod가 디스크에 실제로
//     반영됐는지 - ntfs_driver.cpp의 신규 쓰기 경로 핵심 검증 지점).
//  4. Chmod(owner-write 복원) -> None 기대(fixup USN 재계산이 반복
//     토글에도 안전한지 확인).
//  5. 재-Stat -> mode=0644로 되돌아왔는지 확인.
//
// exitCode: 0=전체 성공. 2=초기 Stat 실패(재시도 예산 소진),
// 4/5/6/7/8=초기 Stat의 type/uid/gid/mode/size 불일치, 9=1차 Chmod
// submit/wait 실패, 11=1차 Chmod error!=None, 14=1차 재-Stat 실패,
// 15=1차 재-Stat mode 불일치(쓰기가 실제로 반영 안 됨), 16=2차 Chmod
// submit/wait 실패, 18=2차 Chmod error!=None, 21=2차 재-Stat 실패,
// 22=2차 재-Stat mode 불일치(fixup 재계산 왕복 실패).
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kTestFilePath[] = "/sys/mnt/NTFSTEST.TXT";
constexpr mc::uint32_t kMaxRetriesForBootRace = 200;

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

mc::StatArgs kStatFile() {
    mc::StatArgs statArgs;
    statArgs.path = kTestFilePath;
    statArgs.pathLen = sizeof(kTestFilePath) - 1;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointStat, &statArgs);
    if (token == 0 || !mc::wait(token)) {
        statArgs.error = mc::ChannelError::ServiceUnavailable;
    }
    return statArgs;
}

}  // namespace

extern "C" void _start() {
    mc::StatArgs statArgs;
    for (mc::uint32_t attempt = 0; attempt < kMaxRetriesForBootRace; ++attempt) {
        statArgs = kStatFile();
        if (statArgs.error == mc::ChannelError::None) {
            break;
        }
    }
    if (statArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }
    if (statArgs.type != mc::FileType::Regular) {
        kFinish(4);
    }
    if (statArgs.uid != 0) {
        kFinish(5);
    }
    if (statArgs.gid != 0) {
        kFinish(6);
    }
    const mc::uint16_t kModeWritable = mc::kPermOwnerRead | mc::kPermOwnerWrite | (1u << 5) | (1u << 2);   // 0644
    const mc::uint16_t kModeReadOnly = mc::kPermOwnerRead | (1u << 5) | (1u << 2);                          // 0444
    if (statArgs.mode != kModeWritable) {
        kFinish(7);
    }
    if (statArgs.size != 23) {
        kFinish(8);
    }

    // 2) Chmod로 owner-write 제거.
    mc::ChmodArgs chmodArgs;
    chmodArgs.path = kTestFilePath;
    chmodArgs.pathLen = sizeof(kTestFilePath) - 1;
    chmodArgs.mode = mc::kPermOwnerRead;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointChmod, &chmodArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(9);
    }
    if (chmodArgs.error != mc::ChannelError::None) {
        kFinish(11);
    }

    // 3) 재-Stat - 실제로 디스크에 반영됐는지 확인.
    statArgs = kStatFile();
    if (statArgs.error != mc::ChannelError::None) {
        kFinish(14);
    }
    if (statArgs.mode != kModeReadOnly) {
        kFinish(15);
    }

    // 4) Chmod로 owner-write 복원 - fixup USN 재계산이 반복 토글에도 안전한지.
    mc::ChmodArgs chmodBackArgs;
    chmodBackArgs.path = kTestFilePath;
    chmodBackArgs.pathLen = sizeof(kTestFilePath) - 1;
    chmodBackArgs.mode = mc::kPermOwnerRead | mc::kPermOwnerWrite;
    token = mc::submit(mc::kSyscallEndpointChmod, &chmodBackArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(16);
    }
    if (chmodBackArgs.error != mc::ChannelError::None) {
        kFinish(18);
    }

    // 5) 최종 재-Stat.
    statArgs = kStatFile();
    if (statArgs.error != mc::ChannelError::None) {
        kFinish(21);
    }
    if (statArgs.mode != kModeWritable) {
        kFinish(22);
    }

    kFinish(0);
}
