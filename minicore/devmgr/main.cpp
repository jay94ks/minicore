// minicore/devmgr: SP-9DD4F3EA §6("devmgr 메인 서비스 시퀀스")의 첫
// 실코드(PN-BD9AAE2F 3번/4번 항목). 아직 §3.2(드라이버 매칭 - 실제
// 드라이버가 하나도 없어 스켈레톤조차 검증할 대상이 없음)/§3.4
// (핫플러그)는 없다 - 이번 증분은 §6 2단계(EnumerateDevices)에 더해
// §3.3(RequestIoPermission)까지 - 각 장치의 첫 번째 메모리 매핑 BAR
// (mmioBases[0])가 있으면 그 권한을 요청해 본다(아직 드라이버가
// 없어 "무엇을 위해" 권한을 쥐는지는 없지만, syscall 자체의 왕복은
// 이렇게 검증한다). minicore/init과 같은 이유로 "libmc를 통해서만
// 커널에 요청한다"는 모양부터 갖추고 다음 증분(§4a-2 설정 로드 -
// fs Open/Read syscall 대기 중이라 아직 착수 불가)이 이어붙인다.
#include "libmc/pnp.h"
#include "libmc/syscall.h"

namespace {

// v1 상한 - 실측 후 조정(RM-23F4B687 §4, kernel/pnp.cpp의
// kMaxCachedPciDevices=256과는 별개로 devmgr 자신의 로컬 캐시 크기).
constexpr mc::uint32_t kMaxDevices = 64;
mc::DeviceDescriptor gDevices[kMaxDevices];
mc::uint32_t gDeviceCount = 0;

// RequestIoPermission 결과(장치 인덱스별) - 아직 로그 출력 syscall이
// 없어 커널 쪽에서 TEMP breadcrumb으로 확인하는 용도(§3.3 검증).
mc::uint64_t gMappedAddr[kMaxDevices] = {};
mc::ChannelError gIoPermError[kMaxDevices] = {};
bool gIoPermAttempted[kMaxDevices] = {};

}  // namespace

extern "C" void _start() {
    mc::EnumerateDevicesArgs args;
    args.startIndex = 0;
    args.capacity = kMaxDevices;
    args.outDevices = gDevices;

    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointEnumerateDevices, &args);
    if (token != 0 && mc::wait(token) && args.error == mc::ChannelError::None) {
        gDeviceCount = args.capacity;  // capacity는 EnumerateDevices onExec()이 "실제로 채운 개수"로 덮어쓴다
    }

    for (mc::uint32_t i = 0; i < gDeviceCount; ++i) {
        if (gDevices[i].mmioBases[0] == 0) {
            continue;  // 메모리 매핑 BAR0이 없는 장치(예: 호스트 브리지) - 스킵
        }
        mc::RequestIoPermissionArgs ioArgs;
        ioArgs.bus = gDevices[i].bus;
        ioArgs.device = gDevices[i].device;
        ioArgs.function = gDevices[i].function;
        ioArgs.mmioBase = gDevices[i].mmioBases[0];

        mc::SyscallToken ioToken = mc::submit(mc::kSyscallEndpointRequestIoPermission, &ioArgs);
        gIoPermAttempted[i] = true;
        if (ioToken != 0 && mc::wait(ioToken)) {
            gMappedAddr[i] = ioArgs.mappedVirtualAddr;
            gIoPermError[i] = ioArgs.error;
        } else {
            gIoPermError[i] = mc::ChannelError::InvalidHandle;  // submit/wait 자체 실패 - 결과 없음을 표시
        }
    }

    // TODO(PN-BD9AAE2F 다음 증분): §4a-2(설정 로드)/§3.2(드라이버
    // 매칭)/§5(드라이버 자식 스폰)/§7(핫플러그 대기)가 이어붙을 자리 -
    // 지금은 gDevices/gMappedAddr/gIoPermError에 실제로 채워지는지만
    // 확인 대상(다음 세션이 QEMU에서 검증). 유저랜드에 로그 출력
    // syscall이 아직 없어(fs/tty 서비스 미착수) 이 자리에서 직접
    // 관측할 방법이 없다 - 종료 후 커널 쪽에서 확인하는 임시
    // 방법(TEMP 로그 breadcrumb)으로 검증한다.
    mc::selfTerminate(0);
}
