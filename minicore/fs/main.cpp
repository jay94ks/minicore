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
//
// **[신규, 2026-09-20, QU-1FB6A7A4 답변 - "블록 디바이스는 그냥 아예
// fs한테 던져버려. 인식/인식 해제까지 전부."]** 블록 스토리지 장치
// (AHCI 등)의 PCI 열거/매칭/드라이버 구동을 devmgr이 아니라 이
// 프로세스가 직접 수행한다 - devmgr이 fork()로 스폰한 자식이 개설한
// 이름 없는 Channel을 이 프로세스가 나중에 찾아야 하는 문제
// (SP-C2670F69 §3.1이 남겨 둔 설계 공백) 자체가 이 결정으로 사라진다
// (같은 프로세스 안이므로 핸드오프가 필요 없음). AHCI 실제 하드웨어
// 코드는 ahci.h/ahci.cpp(PN-4E6EA13D/PN-F60E405A, devmgr에서 이관) -
// `AhciBlockDevice`(block_device.h `fs::BlockDevice` 구현)까지 이
// 증분에서 구성하지만, 그걸 실제 `FileSystemDriver::mount()`(§3.1a,
// libext4/libvfat 자체가 아직 미구현)에 넘기는 건 여전히 범위 밖 -
// PN-452FF696 항목5.
#include "ahci.h"
#include "libmc/channel.h"
#include "libmc/pnp.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

// [신규, QU-1FB6A7A4] devmgr/main.cpp에 있던 것과 동일한 패턴(이관) -
// v1 매칭 후보는 AHCI(classCode=1 "Mass Storage"/subclass=6 "SATA")
// 하나뿐. progIf(AHCI 1.0=1)는 아직 구분 안 함.
constexpr mc::uint32_t kPciClassMassStorage = 1;
constexpr mc::uint32_t kPciSubclassSata = 6;
constexpr mc::uint32_t kMaxDevices = 64;

bool kMatchesAhci(const mc::DeviceDescriptor& dev) {
    return dev.classCode == kPciClassMassStorage && dev.subclass == kPciSubclassSata;
}

// `DeviceDescriptor::mmioBases[6]`은 BAR 인덱스 그대로(mmioBases[0]=
// BAR0 ... mmioBases[5]=BAR5) - AHCI의 ABAR는 관례상 BAR5(실측: QEMU
// ich9-ahci). 첫 번째 실제로 존재하는(0이 아닌) BAR를 찾아 쓴다.
mc::uint64_t kFirstMmioBase(const mc::DeviceDescriptor& dev) {
    for (mc::uint64_t base : dev.mmioBases) {
        if (base != 0) {
            return base;
        }
    }
    return 0;
}

// [신규, QU-1FB6A7A4] fs 프로세스 하나당 AHCI 컨트롤러/블록 장치
// 인스턴스는 최대 1개(v1 - 컨트롤러 여러 개/포트 여러 개를 각각
// BlockDevice로 노출하는 건 후속 과제, 지금은 §3.1 "최소한의 실제
// I/O 검증" 수준). 파일 스코프 전역인 이유는 devmgr에서 쓰던 것과
// 동일(함수-지역 static의 초기화 가드가 이 프리스탠딩 툴체인엔 없음).
ahci::AhciController gAhciController;
ahci::AhciBlockDevice gAhciBlockDevice;
bool gHasAhciBlockDevice = false;

// [신규, QU-1FB6A7A4] devmgr에서 하던 EnumerateDevices→매칭→
// RequestIoPermission→HBA 초기화까지 그대로 이 프로세스 안에서
// 수행한다 - fork() 없이 같은 주소공간이라 Channel 핸드오프 자체가
// 필요 없다. 실패해도(장치 없음/권한 실패) 이 서비스는 계속
// 살아있어야 한다(§3.2 "살아있는 서비스" 계약과 동일한 원칙, VFS
// 마운트 지점 라우팅 자체는 블록 장치 유무와 무관하게 계속 동작해야
// 하므로).
void kProbeAndInitAhci() {
    mc::DeviceDescriptor devices[kMaxDevices];
    mc::EnumerateDevicesArgs enumArgs;
    enumArgs.startIndex = 0;
    enumArgs.capacity = kMaxDevices;
    enumArgs.outDevices = devices;

    mc::SyscallToken enumToken = mc::submit(mc::kSyscallEndpointEnumerateDevices, &enumArgs);
    mc::uint32_t deviceCount = 0;
    if (enumToken != 0 && mc::wait(enumToken) && enumArgs.error == mc::ChannelError::None) {
        deviceCount = enumArgs.capacity;  // capacity는 onExec()이 "실제로 채운 개수"로 덮어쓴다
    }

    for (mc::uint32_t i = 0; i < deviceCount; ++i) {
        if (!kMatchesAhci(devices[i]) || kFirstMmioBase(devices[i]) == 0) {
            continue;
        }

        mc::RequestIoPermissionArgs ioArgs;
        ioArgs.bus = devices[i].bus;
        ioArgs.device = devices[i].device;
        ioArgs.function = devices[i].function;
        ioArgs.mmioBase = kFirstMmioBase(devices[i]);
        mc::SyscallToken ioToken = mc::submit(mc::kSyscallEndpointRequestIoPermission, &ioArgs);
        if (ioToken != 0) {
            mc::wait(ioToken);
        }
        if (ioArgs.error != mc::ChannelError::None) {
            continue;  // 이 장치 실패 - 다음 후보로(RM-23F4B687 §4, 장치 하나 실패가 서비스 전체를 막으면 안 됨)
        }

        if (!gAhciController.init(ioArgs.mappedVirtualAddr)) {
            continue;
        }

        ahci::PortProbeResult probeResult;
        ahci::AhciPort* port = nullptr;
        if (gAhciController.probeFirstDevice(&probeResult, &port)) {
            gAhciBlockDevice.init(port, probeResult.identifyData);
            gHasAhciBlockDevice = true;
        }
        return;  // v1은 첫 매칭 성공 장치 하나만(§3.1 최소 검증 범위)
    }
}

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

    // [신규, QU-1FB6A7A4] 블록 스토리지 장치 인식/구동 - VFS 마운트
    // 지점 라우팅 등록 이후, accept 루프 진입 전에 한 번(§3.2 "살아있는
    // 서비스" 계약과 동일하게 실패해도 이 프로세스는 계속 존재).
    kProbeAndInitAhci();

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
