// minicore/sockclient - minicore/socktest(PN-CC0F4EAC 소켓 계층 실측
// 검증)의 클라이언트 반쪽 - 독립된 프로세스로서 kmain.cpp가 socktest와
// 거의 동시에(init/pubreg/authmgr가 서로 그렇듯 커널이 각자 독립
// execImage로) 스폰한다. socktest/main.cpp 상단 주석 참고 - CreateThread
// (한 프로세스 두 스레드)/SpawnProcess(부모가 자식을 낳고 곧바로 자기
// 다음 syscall을 제출)는 둘 다 "두 Task가 진짜 동시에 syscall을
// 주고받는" 조합에서 커널 핸들러가 아예 호출 안 되는 재현 가능한
// 증상(PN-395F4D89)이 나와, 이 프로젝트가 부팅 때부터 검증해 온
// "완전히 독립된 프로세스 둘이 이름으로 서로를 찾아 연결"하는
// 패턴으로 우회했다.
//
// **socktest보다 먼저 Connect를 시도할 수 있다** - 이 둘 사이엔
// init/pubreg처럼 SignalUserlandReady류 조정 채널이 없으므로, socktest
// 의 Bind()가 아직 안 끝났으면 NotFound가 날 수 있다 - 몇 차례
// 재시도한다(dbgdriver의 kPollGetRegisters/kPollDebugContinue와 동일한
// 이유의 폴링).
//
// exitCode: 0=전 구간 성공, 1=Socket 실패, 2=Connect 실패,
// 3=Write 실패, 4=[신규, 2026-09-27] Shutdown(Write) 실패,
// 5=[신규] Shutdown(Write) 이후 Write가 BrokenPipe가 아님(핸들이
// 여전히 유효해야 하는데 다른 에러가 났거나 성공해 버림).
#include "libmc/socket.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kSockName[] = "socktest.sock";
constexpr mc::uint32_t kSockNameLen = sizeof(kSockName) - 1;
constexpr char kMsg[] = "hello-socket";
constexpr mc::uint32_t kMsgLen = sizeof(kMsg) - 1;
constexpr mc::uint32_t kMaxConnectAttempts = 200000;

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

}  // namespace

extern "C" void _start() {
    mc::SocketArgs sockB;
    sockB.domain = mc::SocketDomain::Unix;
    sockB.type = mc::SocketType::Stream;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSocket, &sockB);
    if (token == 0 || !mc::wait(token) || sockB.error != mc::ChannelError::None) {
        kFinish(1);
    }

    // socktest의 Bind()가 아직 안 끝났을 수 있어(위 파일 상단 주석)
    // NotFound에 한해 재시도한다 - 그 외 에러는 재시도해도 의미 없다.
    mc::SocketConnectArgs connectArgs;
    bool connected = false;
    for (mc::uint32_t attempt = 0; attempt < kMaxConnectAttempts; ++attempt) {
        connectArgs = mc::SocketConnectArgs{};
        connectArgs.fd = static_cast<mc::int32_t>(sockB.fd);
        connectArgs.path = kSockName;
        connectArgs.pathLen = kSockNameLen;
        token = mc::submit(mc::kSyscallEndpointSocketConnect, &connectArgs);
        if (token == 0 || !mc::wait(token)) {
            kFinish(2);
        }
        if (connectArgs.error == mc::ChannelError::None) {
            connected = true;
            break;
        }
        if (connectArgs.error != mc::ChannelError::NotFound) {
            kFinish(2);
        }
    }
    if (!connected) {
        kFinish(2);
    }

    mc::WriteArgs writeArgs;
    writeArgs.fd = static_cast<mc::int32_t>(sockB.fd);
    writeArgs.buf = kMsg;
    writeArgs.len = kMsgLen;
    token = mc::submit(mc::kSyscallEndpointWrite, &writeArgs);
    if (token == 0 || !mc::wait(token) || writeArgs.error != mc::ChannelError::None ||
        writeArgs.bytesWritten != kMsgLen) {
        kFinish(3);
    }

    // [신규, 2026-09-27, SP-231493CB §5, Shutdown 구현 검증] Shutdown(Write) -
    // 이 fd는 계속 열려 있어야 한다(Close와 달리 InvalidHandle이 아니라
    // BrokenPipe로 구분돼야 함, 바로 아래 재확인).
    mc::SocketShutdownArgs shutdownArgs;
    shutdownArgs.fd = static_cast<mc::int32_t>(sockB.fd);
    shutdownArgs.how = mc::ShutdownHow::Write;
    token = mc::submit(mc::kSyscallEndpointSocketShutdown, &shutdownArgs);
    if (token == 0 || !mc::wait(token) || shutdownArgs.error != mc::ChannelError::None) {
        kFinish(4);
    }
    // Shutdown(Write) 이후의 Write는 반드시 BrokenPipe여야 한다(핸들
    // 자체는 여전히 유효 - InvalidHandle이면 안 됨, kShutdownBridgeWrite가
    // 실제로는 Close처럼 핸들을 제거해 버린 게 아닌지 구분하는 지점).
    writeArgs = mc::WriteArgs{};
    writeArgs.fd = static_cast<mc::int32_t>(sockB.fd);
    writeArgs.buf = kMsg;
    writeArgs.len = kMsgLen;
    token = mc::submit(mc::kSyscallEndpointWrite, &writeArgs);
    if (token == 0 || !mc::wait(token) || writeArgs.error != mc::ChannelError::BrokenPipe) {
        kFinish(5);
    }

    mc::CloseArgs closeArgs;
    closeArgs.fd = static_cast<mc::int32_t>(sockB.fd);
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    if (token != 0) {
        mc::wait(token);
    }

    kFinish(0);
}
