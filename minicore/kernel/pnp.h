#ifndef MINICORE_KERNEL_PNP_H
#define MINICORE_KERNEL_PNP_H

#include "channel.h"
#include "libkenv/types.h"
#include "syscall.h"

namespace kernel {

// 장치 자동 인식/핫플러그 프레임워크(SP-9DD4F3EA, "PnP")의 커널 쪽
// syscall 표면 - devmgr(유저랜드, PN-BD9AAE2F)이 하드웨어 토폴로지를
// 조회하는 데 쓴다. §3.2-§3.4(드라이버 매칭/IO 권한 부여/핫플러그)는
// devmgr 자신의 코드와 후속 syscall(RequestIoPermission, RM-48E1E610
// 9번, 아직 미구현)이 다룬다 - 이 파일은 §3.1(장치 열거)만 구현한다.

// [SP-9DD4F3EA §3.1] PCI 장치 하나의 열거 결과 - devmgr이 이 배열을
// 로컬에 캐시해 드라이버 매칭(§3.2)에 쓴다.
struct DeviceDescriptor {
    uint32_t bus = 0, device = 0, function = 0;
    uint32_t vendorId = 0, deviceId = 0;
    uint32_t classCode = 0, subclass = 0, progIf = 0;
    // BAR0~5 중 메모리 매핑인 것만(포트 I/O BAR는 0) - 64비트 BAR의
    // 상위 32비트를 담는 다음 슬롯은 독립된 BAR가 아니므로 0으로
    // 남는다. 아직 어떤 프로세스 주소공간에도 매핑되지 않은 물리주소
    // 그대로다(실제 가상주소 매핑/소유권 배정은 RequestIoPermission,
    // §3.3의 몫 - 이 syscall은 순수 조회만 한다).
    uint64_t mmioBases[6] = {};
    // 0이면 아직 미배정(MSI/MSI-X 협상은 RequestIoPermission 시점에
    // 이뤄진다 - 여기서는 열거만 한다).
    uint32_t irqVector = 0;
};

// [SP-9DD4F3EA §3.1] devmgr이 부팅 후 이 syscall을 반복 호출해 전체
// PCI 장치 목록을 얻는다(페이지네이션 - 결과가 커널 스택/버퍼 크기를
// 넘을 수 있음).
struct EnumerateDevicesArgs {
    // in: 몇 번째 장치부터 반환할지.
    uint32_t startIndex = 0;
    // in/out: 호출부가 준비한 배열 크기(in) / 실제로 채운 개수(out).
    uint32_t capacity = 0;
    DeviceDescriptor* outDevices = nullptr;
    // out: 전체 장치 수(다음 호출의 startIndex 계산용).
    uint32_t totalCount = 0;
    // [이 struct의 첫 실제 구현에서 추가] outDevices가 호출자 자신의
    // 유저 주소공간에 속하지 않으면 ChannelError::InvalidPointer -
    // 새 오류 종류를 만들지 않고 기존 enum을 재사용한다(SP-9DD4F3EA
    // §3.3의 RequestIoPermissionArgs도 이미 ChannelError를 그대로
    // 재사용하는 선례가 있다, PN-B552E75F가 도입한 값 그대로 재사용).
    ChannelError error = ChannelError::None;
};

constexpr SyscallEndpointId kSyscallEndpointEnumerateDevices = 8;

class PnpService {
public:
    // 부팅 시 한 번 호출 - EnumerateDevices endpoint를 등록한다.
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PNP_H
