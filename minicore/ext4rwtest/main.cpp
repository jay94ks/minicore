// minicore/ext4rwtest - PN-2A0981B7 항목5(QU-1E7DFB8C 답변(A), ext4
// 실제 쓰기 가능 마운트 E2E 검증) 전용 클라이언트. diskmetatest와 같은
// ext4 이미지(mkfs.ext4 -O metadata_csum, testfile.txt uid=77/gid=88/
// mode=0640/size=22)가 이번에는 부팅 cmdline "--rw-mount" 플래그로
// fs 커널 서비스의 kTryAutoMountBlockDevice()(fs.cpp)에 의해 실제
// 쓰기 가능하게(readOnly=false) 자동 마운트된 상태를 전제로 한다 -
// MINICORE_QEMU_AHCI=1 + MINICORE_QEMU_AHCI_DISK=<diskmetatest용
// 이미지 경로>로, 커널 cmdline에 "--rw-mount"를 포함해 부팅해야 한다.
//
// diskmetatest는 "읽기 전용 마운트에서 Chmod가 실제로 거부되는지"의
// 영구 회귀 가드라 그 계약(PermissionDenied 기대)을 이 테스트로
// 바꾸지 않는다 - 이 테스트는 정반대 전제(쓰기 가능 마운트)를 검증하는
// 별도 클라이언트다.
//
// 시나리오:
//  1. Stat - 초기 mode=0640, uid=77/gid=88, size=22 확인.
//  2. Chmod(mode=0400) -> None 기대(ext4_driver.cpp의 기존 Chmod 쓰기
//     경로가 readOnly=false 마운트에서 실제로 디스크에 반영되는지 -
//     QU-1E7DFB8C가 실측하지 못했다고 지적한 바로 그 지점). ext4
//     Chmod는 exFAT/FAT류와 달리 요청 mode 하위 비트를 그대로 전부
//     덮어쓴다(실제 POSIX chmod(2) 의미) - 0440이 아니라 0400이 된다.
//  3. 재-Stat -> mode=0400로 바뀌었는지 확인.
//  4. Chmod(mode=0640, 원본 그대로) -> None 기대.
//  5. 재-Stat -> mode=0640로 되돌아왔는지 확인.
//
// exitCode: 0=전체 성공. 2=초기 Stat 실패(재시도 예산 소진),
// 4/5/6/7/8=초기 Stat의 type/uid/gid/mode/size 불일치, 9=1차 Chmod
// submit/wait 실패, 11=1차 Chmod error!=None, 14=1차 재-Stat 실패,
// 15=1차 재-Stat mode 불일치, 16=2차 Chmod submit/wait 실패,
// 18=2차 Chmod error!=None, 21=2차 재-Stat 실패, 22=2차 재-Stat mode
// 불일치.
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kTestFilePath[] = "/sys/mnt/testfile.txt";
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
    if (statArgs.uid != 77) {
        kFinish(5);
    }
    if (statArgs.gid != 88) {
        kFinish(6);
    }
    // [주의] ext4 Chmod는 exFAT/FAT류의 "읽기전용 비트 하나만 토글"과
    // 달리 요청받은 mode 하위 비트를 그대로 전부 덮어쓴다(ext4_driver.cpp
    // ChmodArgs 처리부, 실제 POSIX chmod(2) 의미) - 그래서 아래 기대값은
    // 각 Chmod 호출에 실제로 넘기는 mode 값과 정확히 일치해야 한다
    // (group-read 비트를 안 넘기면 결과에서도 사라짐).
    const mc::uint16_t kModeWritable = mc::kPermOwnerRead | mc::kPermOwnerWrite | (1u << 5);  // 0640(원본)
    const mc::uint16_t kModeReadOnly = mc::kPermOwnerRead;                                    // 0400
    if (statArgs.mode != kModeWritable) {
        kFinish(7);
    }
    if (statArgs.size != 22) {
        kFinish(8);
    }

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

    statArgs = kStatFile();
    if (statArgs.error != mc::ChannelError::None) {
        kFinish(14);
    }
    if (statArgs.mode != kModeReadOnly) {
        kFinish(15);
    }

    mc::ChmodArgs chmodBackArgs;
    chmodBackArgs.path = kTestFilePath;
    chmodBackArgs.pathLen = sizeof(kTestFilePath) - 1;
    chmodBackArgs.mode = kModeWritable;  // 원본 0640 그대로 복원(ext4는 넘긴 비트 그대로 덮어쓰므로)
    token = mc::submit(mc::kSyscallEndpointChmod, &chmodBackArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(16);
    }
    if (chmodBackArgs.error != mc::ChannelError::None) {
        kFinish(18);
    }

    statArgs = kStatFile();
    if (statArgs.error != mc::ChannelError::None) {
        kFinish(21);
    }
    if (statArgs.mode != kModeWritable) {
        kFinish(22);
    }

    kFinish(0);
}
