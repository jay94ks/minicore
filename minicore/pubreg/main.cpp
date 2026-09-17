// minicore/pubreg: "프로세스간 공개 인터페이스" Registry 서비스(5번째
// 커널 서비스, SP-B071E628 §1~§4, PN-185406F6 항목1) - devmgr/init과
// 같은 이유로 "libmc를 통해서만 커널에 요청한다"는 모양부터 갖춰 둔다
// (minicore/devmgr/main.cpp/minicore/init/main.cpp와 동일한 관례).
//
// **[완료, 2026-09-17, PN-185406F6 항목3] "이름 있는 Channel" 개설 +
// accept 루프**: PN-EAB3A9AE가 완성한 `libmc/channel.h` 위에서 부팅
// 즉시 `OpenChannel(name="pubreg")`로 자신의 등록 채널을 개설하고,
// `AcceptFromChannel`로 들어오는 연결을 순서대로 받는다.
//
// **[미착수] 항목4**: register/query 메시지 처리(libjson 파싱 -
// SP-B071E628 §3)는 아직 없다 - 그 프로토콜 자체가 별도의 설계
// 확정을 요구하는 작업량이라(RM-23F4B687 §4 - 검증 없이 한 번에 다
// 만들지 않는다), 이번 증분은 accept 왕복 자체가 실제 syscall 트랩
// 경계에서 올바르게 동작하는지만 검증하고 각 연결을 즉시 닫는다.
#include "libmc/channel.h"
#include "libmc/syscall.h"

namespace {

constexpr char kPubregChannelName[] = "pubreg";

}  // namespace

extern "C" void _start() {
    mc::OpenChannelArgs openArgs;
    openArgs.name = kPubregChannelName;
    openArgs.nameLength = sizeof(kPubregChannelName) - 1;

    mc::SyscallToken openToken = mc::submit(mc::kSyscallEndpointOpenChannel, &openArgs);
    if (openToken == 0 || !mc::wait(openToken) || openArgs.error != mc::ChannelError::None) {
        // 이름 충돌(이미 다른 pubreg 인스턴스가 떠 있음, SP-9DD4F3EA
        // §4a-3류 "인스턴스는 무조건 1개" 위반) 또는 자원 고갈 - 이
        // 서비스는 계속 존재할 이유가 없으므로 종료한다(essential
        // service 정책상 이 종료는 상위에서 패닉으로 처리됨, 의도된
        // 동작 - PN-F82B59FD).
        mc::selfTerminate(1);
    }

    for (;;) {
        mc::AcceptFromChannelArgs acceptArgs;
        acceptArgs.channelHandle = openArgs.channelHandle;

        mc::SyscallToken acceptToken = mc::submit(mc::kSyscallEndpointAcceptFromChannel, &acceptArgs);
        if (acceptToken == 0 || !mc::wait(acceptToken) || acceptArgs.error != mc::ChannelError::None) {
            // 채널이 소멸됐거나(NotFound) 복구 불가능한 상태 - 더 이상
            // accept할 수 없으므로 루프를 끝낸다.
            break;
        }

        // TODO(PN-185406F6 항목4): 여기서 ChannelRead로 register/query
        // 메시지를 받아 libjson으로 파싱하고 내부 테이블에 반영한 뒤
        // ChannelWrite로 응답해야 한다. 그 프로토콜이 아직 없어 이번
        // 증분은 연결만 받고 바로 닫는다(accept 왕복 자체의 실측
        // 검증 목적, PN-EAB3A9AE).
        mc::CloseBridgeArgs closeArgs;
        closeArgs.bridge = acceptArgs.bridge;
        mc::SyscallToken closeToken = mc::submit(mc::kSyscallEndpointCloseBridge, &closeArgs);
        if (closeToken != 0) {
            mc::wait(closeToken);
        }
    }

    mc::selfTerminate(0);
}
