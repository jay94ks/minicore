// minicore/fatdisktest - PN-2A0981B7 항목3(FAT32 디스크 이미지
// Stat/Chmod 실측 검증) 전용 클라이언트. diskmetatest(ext4용)와 짝을
// 이루는 FAT32용 - 같은 부팅에 ext4/FAT32 이미지를 동시에 붙일 수
// 없으므로(fs.cpp의 kTryAutoMountBlockDevice() 우선순위 체인은 장치
// 하나만 다룬다) 별도 클라이언트로 분리했다. 실제 mkfs.vfat -F 32
// 이미지(mtools mcopy로 FATTEST.TXT를 넣고 mattrib +r로 읽기전용
// 속성을 세움)가 fs 커널 서비스에 의해 /sys/mnt에 읽기 전용으로
// 자동 마운트된 상태를 전제로 한다 - MINICORE_QEMU_AHCI=1 +
// MINICORE_QEMU_AHCI_DISK=<이미지 경로>로 부팅해야 한다.
//
// 시나리오:
//  1. Stat("/sys/mnt/FATTEST.TXT") - FAT32는 소유자 개념이 없어
//     uid=gid=mountUid/mountGid(커널 자동 마운트는 0/0) 그대로
//     반사, mode는 ReadOnly 속성 하나로부터 0444로 합성(§3.3).
//  2. Chmod - vfat_driver.cpp는 이미 Mkdir 등과 동일하게 readOnly_
//     게이트를 갖고 있음(ext4와 달리 diskmetatest 검증 중 회귀가
//     없었음, commit 8e168fe 참고) - PermissionDenied 기대, 이
//     시나리오는 그 사실의 영구 회귀 검증이다.
//
// exitCode: 0=전체 성공, 1-7=Stat 단계, 8-9=Chmod 단계.
//
// [부팅 경합] diskmetatest와 동일한 이유(fs KernelThread의 비동기
// AHCI 인식+마운트 완료를 기다려야 함) - Stat 단계만 재시도 예산으로
// 흡수한다.
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kTestFilePath[] = "/sys/mnt/FATTEST.TXT";
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
    if (statArgs.uid != 0) {
        kFinish(4);
    }
    if (statArgs.gid != 0) {
        kFinish(5);
    }
    const mc::uint16_t kExpectedMode = mc::kPermOwnerRead | (1u << 5) | (1u << 2);  // 0444
    if (statArgs.mode != kExpectedMode) {
        kFinish(6);
    }
    if (statArgs.size != 26) {
        kFinish(7);
    }

    mc::ChmodArgs chmodArgs;
    chmodArgs.path = kTestFilePath;
    chmodArgs.pathLen = sizeof(kTestFilePath) - 1;
    chmodArgs.mode = mc::kPermOwnerRead | mc::kPermOwnerWrite;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointChmod, &chmodArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(8);
    }
    if (chmodArgs.error != mc::ChannelError::PermissionDenied) {
        kFinish(9);
    }

    kFinish(0);
}
