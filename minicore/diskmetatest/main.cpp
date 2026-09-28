// minicore/diskmetatest - PN-2A0981B7 항목3(ext4/FAT류 Stat/Chmod/Chown
// 디스크 이미지 실측 검증) 전용 클라이언트. vfsmetatest(livefs 전용)와
// 달리 실제 AHCI 디스크 이미지(mkfs.ext4 -O metadata_csum, debugfs로
// uid=77/gid=88/mode=0640 파일 하나를 심어 둠)가 fs 커널 서비스의
// kTryAutoMountBlockDevice()(fs.cpp)에 의해 /sys/mnt에 읽기 전용으로
// 자동 마운트된 상태를 전제로 한다 - MINICORE_QEMU_AHCI=1 +
// MINICORE_QEMU_AHCI_DISK=<이미지 경로>로 부팅해야 한다.
//
// 시나리오:
//  1. Stat("/sys/mnt/testfile.txt") - uid=77/gid=88/mode=0640/size=22
//     실제 온디스크 값이 정확히 노출되는지 확인.
//  2. Chmod("/sys/mnt/testfile.txt", ...) - PermissionDenied 기대.
//     [배경] 이 검증 도중 ext4_driver.cpp의 Chmod/Chown 분기가
//     Mkdir/Write/Unlink/Rmdir와 달리 readOnly_ 게이트가 아예 빠져
//     있어서 읽기 전용 마운트인데도 실제로 디스크에 써지는 회귀를
//     발견해 고쳤다(libvfat의 동일 분기는 이미 이 게이트를 갖고
//     있었음) - 이 시나리오가 바로 그 회귀에 대한 영구 검증이다.
//
// exitCode: 0=전체 성공, 1-7=Stat 단계, 8-9=Chmod 단계.
//
// [부팅 경합] fs KernelThread의 kTryAutoMountBlockDevice()(fs.cpp)는
// PCI 열거+AHCI 초기화+슈퍼블록 판별을 전부 비동기로 마친 뒤에야
// /sys/mnt를 실제 KernelDriver로 바꿔 끼운다 - 그 전에 이 테스트가
// 먼저 Stat을 쏘면 아직 이름 없는 Channel 마운트 스텁 상태라 실패할
// 수 있다(createusertest/granttest 등이 이미 겪은 것과 같은 종류의
// 부팅 경합) - Stat 단계만 작은 재시도 예산으로 흡수한다.
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kTestFilePath[] = "/sys/mnt/testfile.txt";
constexpr mc::uint32_t kMaxRetriesForBootRace = 200;

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

}  // namespace

extern "C" void _start() {
    mc::StatArgs statArgs;
    for (mc::uint32_t attempt = 0; attempt < kMaxRetriesForBootRace; ++attempt) {
        statArgs = mc::StatArgs{};
        statArgs.path = kTestFilePath;
        statArgs.pathLen = sizeof(kTestFilePath) - 1;
        mc::SyscallToken token = mc::submit(mc::kSyscallEndpointStat, &statArgs);
        if (token == 0 || !mc::wait(token)) {
            kFinish(1);
        }
        if (statArgs.error == mc::ChannelError::None) {
            break;
        }
    }
    if (statArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }
    if (statArgs.type != mc::FileType::Regular) {
        kFinish(3);
    }
    if (statArgs.uid != 77) {
        kFinish(4);
    }
    if (statArgs.gid != 88) {
        kFinish(5);
    }
    const mc::uint16_t kExpectedMode = mc::kPermOwnerRead | mc::kPermOwnerWrite | (1u << 5);  // 0640
    if (statArgs.mode != kExpectedMode) {
        kFinish(6);
    }
    if (statArgs.size != 22) {
        kFinish(7);
    }

    mc::ChmodArgs chmodArgs;
    chmodArgs.path = kTestFilePath;
    chmodArgs.pathLen = sizeof(kTestFilePath) - 1;
    chmodArgs.mode = mc::kPermOwnerRead;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointChmod, &chmodArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(8);
    }
    if (chmodArgs.error != mc::ChannelError::PermissionDenied) {
        kFinish(9);
    }

    kFinish(0);
}
