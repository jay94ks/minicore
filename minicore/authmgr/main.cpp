// minicore/authmgr: 사용자 신원 관리(authmgr) 서비스(6번째 커널
// 서비스, SP-8B6B8D25 §3.1 항목8/SP-30FCC8AE §1-D, PN-24A2B6F5/
// PN-CFEAEF40) - devmgr/init/pubreg와 같은 이유로 "libmc를 통해서만
// 커널에 요청한다"는 모양부터 갖춰 둔다(minicore/pubreg/main.cpp와
// 완전히 동일한 관례).
//
// **[범위, PN-CFEAEF40] 이 파일은 스캐폴딩만 다룬다** - 실제
// Request/Response/Notification 바이너리 프로토콜(libkproto로 추출될
// 대상, SP-30FCC8AE §1-D)과 libkvdb 연동, sudo/su 판정 로직은 전부
// PN-24A2B6F5의 후속 세션이 이어간다. v1은 accept한 연결을 어떤
// 프로토콜 처리도 없이 즉시 close한다 - pubreg가 PN-185406F6 항목1~3
// 단계에서 그랬던 것과 동일한 순서(서비스 골격을 먼저 세우고, 그
// 위에 실제 프로토콜을 나중에 얹는다).
#include "libmc/channel.h"
#include "libmc/syscall.h"

namespace {

constexpr char kAuthmgrChannelName[] = "authmgr";

}  // namespace

extern "C" void _start() {
    mc::OpenChannelArgs openArgs;
    openArgs.name = kAuthmgrChannelName;
    openArgs.nameLength = sizeof(kAuthmgrChannelName) - 1;

    mc::SyscallToken openToken = mc::submit(mc::kSyscallEndpointOpenChannel, &openArgs);
    if (openToken == 0 || !mc::wait(openToken) || openArgs.error != mc::ChannelError::None) {
        // 이름 충돌(이미 다른 authmgr 인스턴스가 떠 있음) 또는 자원
        // 고갈 - 이 서비스는 계속 존재할 이유가 없으므로 종료한다
        // (pubreg의 동일 실패 처리와 같은 이유).
        mc::selfTerminate(1);
    }
    mc::BridgeHandle serverChannel = openArgs.channelHandle;

    for (;;) {
        mc::AcceptFromChannelArgs acceptArgs;
        acceptArgs.channelHandle = serverChannel;
        mc::SyscallToken acceptToken = mc::submit(mc::kSyscallEndpointAcceptFromChannel, &acceptArgs);
        if (acceptToken == 0 || !mc::wait(acceptToken)) {
            break;  // 복구 불가능한 트랩 실패 - 서비스를 끝낸다.
        }
        if (acceptArgs.error != mc::ChannelError::None) {
            // 채널이 소멸됐거나(NotFound) 복구 불가능 - 더 이상
            // accept할 수 없으므로 루프를 끝낸다.
            break;
        }

        // v1 스캐폴딩 - 실제 프로토콜 처리 없이 즉시 close(위 파일
        // 상단 주석 참고). 후속 세션이 이 자리에 Request/Response/
        // Notification 처리를 얹는다.
        mc::CloseBridgeArgs closeArgs;
        closeArgs.bridge = acceptArgs.bridge;
        mc::SyscallToken closeToken = mc::submit(mc::kSyscallEndpointCloseBridge, &closeArgs);
        if (closeToken != 0) {
            mc::wait(closeToken);
        }
    }

    mc::selfTerminate(0);
}
