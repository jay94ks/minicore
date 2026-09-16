// minicore/devmgr: SP-9DD4F3EA §6("devmgr 메인 서비스 시퀀스")의 첫
// 실코드(PN-BD9AAE2F 3번 항목). 아직 §3.2(드라이버 매칭)/§3.3(IO 권한
// 부여)/§3.4(핫플러그)는 없다 - 이번 증분은 §6 2단계(EnumerateDevices
// 반복 호출로 PCI 토폴로지를 로컬에 캐시)까지만 - minicore/init과
// 같은 이유로 "libmc를 통해서만 커널에 요청한다"는 모양부터 갖추고
// 다음 증분(§4a-2 설정 로드부터)이 이어붙인다.
#include "libmc/pnp.h"
#include "libmc/syscall.h"

namespace {

// v1 상한 - 실측 후 조정(RM-23F4B687 §4, kernel/pnp.cpp의
// kMaxCachedPciDevices=256과는 별개로 devmgr 자신의 로컬 캐시 크기).
constexpr mc::uint32_t kMaxDevices = 64;
mc::DeviceDescriptor gDevices[kMaxDevices];
mc::uint32_t gDeviceCount = 0;

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

    // TODO(PN-BD9AAE2F 3번 항목 다음 증분): §4a-2(설정 로드)/§3.2(드라이버
    // 매칭)/§5(드라이버 자식 스폰)/§7(핫플러그 대기)가 이어붙을 자리 -
    // 지금은 gDevices/gDeviceCount에 실제로 채워지는지만 확인 대상
    // (다음 세션이 QEMU에서 검증). 유저랜드에 로그 출력 syscall이 아직
    // 없어(fs/tty 서비스 미착수) 이 자리에서 직접 관측할 방법이 없다 -
    // 종료 후 커널 쪽에서 확인하는 임시 방법(TEMP 로그 breadcrumb)으로
    // 검증한다.
    mc::selfTerminate(0);
}
