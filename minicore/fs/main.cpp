// minicore/fs: VFS 마운트 지점의 실질 처리를 담당하는 커널 서비스
// (SP-8B6B8D25 §4-A, PN-452FF696 항목3) - devmgr/pubreg/init과 같은
// 이유("libmc를 통해서만 커널에 요청한다"는 모양부터 갖춘다)로 신설.
//
// **[완료, 이 파일] SP-8B6B8D25 §4 마운트 지점 목록 사전 등록**: 부팅
// 즉시 이름 없는 Channel을 하나 개설하고(§2.2 - VFS 라우팅은 이름이
// 아니라 MountTable의 channelId로 이뤄지므로 pubreg류 "이름 있는
// Channel"과 달리 이름이 필요 없다), §4가 명시적으로 "일반 FS"로
// 태그한 네 개의 최상위 지점(/sys/etc, /sys/bin, /sys/mnt, /sys/tmp)
// 을 그 Channel로 Mount한다.
//
// **[정직하게 기록, 이 증분의 스코프 결정]** §4가 예시로 든 나머지
// 후보 - `usr/*/mnt`(사용자별 경로, 아직 사용자 계정/`/usr/<name>`
// 생성 모델 자체가 없음)와 `/boot/uefi`(UEFI 부팅 경로 전용, 이
// 프로젝트는 아직 PVH/multiboot2만 지원 - PN-7FBF255A 미착수) -는
// 이번 증분에서 제외했다. `/sys/dev`(devmgr 담당)/`/sys/live`(커널
// 자신이 이미 `mountKernel()`로 직접 마운트, SP-7CC5693A §2.4)는
// 애초에 이 서비스의 대상이 아니다. 각 Mount 호출의 성공/실패는
// 확인하지 않는다(유저랜드에 로그 출력 syscall이 아직 없어 - fs/tty
// 서비스 자기 자신이 그 인프라라 순환 의존 - 여기서 관측할 방법이
// 없다, devmgr/pubreg와 동일한 한계).
//
// **[미착수] 실제 파일시스템 드라이버 연결(§3.1a/§3.2)과 Open/Read
// 프로토콜(§9, SP-2AAD7C8D)**: 이 네 마운트 지점 뒤에 아직 어떤
// 실제 파일시스템도 없다(libext4/libswapfs/libvfat 전부 "예정,
// 미구현") - 이번 증분은 각 지점이 라우팅 테이블에 정확히 등록되고
// 그 소유 Channel의 accept 왕복이 실제 syscall 트랩 경계에서 동작
// 하는지만 검증한다(accept 즉시 close, pubreg 항목3과 동일 패턴).
#include "libmc/channel.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

struct MountPointSpec {
    const char* path;
    mc::uint32_t pathLen;
};

constexpr char kMountEtc[] = "/sys/etc";
constexpr char kMountBin[] = "/sys/bin";
constexpr char kMountMnt[] = "/sys/mnt";
constexpr char kMountTmp[] = "/sys/tmp";

constexpr MountPointSpec kMountPoints[] = {
    {kMountEtc, sizeof(kMountEtc) - 1},
    {kMountBin, sizeof(kMountBin) - 1},
    {kMountMnt, sizeof(kMountMnt) - 1},
    {kMountTmp, sizeof(kMountTmp) - 1},
};
constexpr mc::uint32_t kMountPointCount = sizeof(kMountPoints) / sizeof(kMountPoints[0]);

}  // namespace

extern "C" void _start() {
    mc::OpenChannelArgs openArgs;  // name 없이 개설 - VFS 라우팅은 MountTable의 channelId로 이뤄진다(§2.2)
    mc::SyscallToken openToken = mc::submit(mc::kSyscallEndpointOpenChannel, &openArgs);
    if (openToken == 0 || !mc::wait(openToken) || openArgs.error != mc::ChannelError::None) {
        // 자원 고갈 등 - 이 서비스는 계속 존재할 이유가 없다(essential
        // service 정책상 상위에서 패닉으로 처리됨, devmgr/pubreg와 동일).
        mc::selfTerminate(1);
    }

    for (mc::uint32_t i = 0; i < kMountPointCount; ++i) {
        mc::MountArgs mountArgs;
        mountArgs.path = kMountPoints[i].path;
        mountArgs.pathLen = kMountPoints[i].pathLen;
        mountArgs.channelId = openArgs.channelId;
        mc::SyscallToken mountToken = mc::submit(mc::kSyscallEndpointMount, &mountArgs);
        if (mountToken != 0) {
            mc::wait(mountToken);
        }
    }

    for (;;) {
        mc::AcceptFromChannelArgs acceptArgs;
        acceptArgs.channelHandle = openArgs.channelHandle;

        mc::SyscallToken acceptToken = mc::submit(mc::kSyscallEndpointAcceptFromChannel, &acceptArgs);
        if (acceptToken == 0 || !mc::wait(acceptToken) || acceptArgs.error != mc::ChannelError::None) {
            break;
        }

        // TODO(§9 Open/Read 프로토콜, SP-2AAD7C8D - 아직 미확정): 실제
        // 파일 열기/읽기/쓰기는 여기서 ChannelRead/ChannelWrite로
        // 처리해야 하지만 그 프로토콜 자체가 없어 연결만 받고 바로
        // 닫는다(accept 왕복 자체의 실측 검증 목적, pubreg 항목3과
        // 동일 패턴).
        mc::CloseBridgeArgs closeArgs;
        closeArgs.bridge = acceptArgs.bridge;
        mc::SyscallToken closeToken = mc::submit(mc::kSyscallEndpointCloseBridge, &closeArgs);
        if (closeToken != 0) {
            mc::wait(closeToken);
        }
    }

    mc::selfTerminate(0);
}
