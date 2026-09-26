// minicore/socktest - PN-CC0F4EAC(소켓 계층, AF_UNIX) 전용 최소 실측
// 검증 프로그램(서버 역할) - minicore/sockclient(클라이언트 역할)와
// 짝을 이룬다. 서로 다른 프로세스로서 부팅 시점에 kmain.cpp가 각자
// 독립적으로(SpawnProcess syscall 경유 없이, init/pubreg/authmgr와
// 완전히 동일한 "커널이 직접 execImage" 패턴으로) 스폰한다.
//
// **왜 SpawnProcess(한 프로세스가 다른 프로세스를 낳음)나
// CreateThread(한 프로세스 두 스레드)가 아니라 "커널이 둘 다 독립
// 스폰"인가(실측으로 발견, 2026-09-27)** - 두 가지를 순서대로
// 시도했었다:
//   1) 이 프로세스 안에서 CreateThread로 두 번째 스레드를 만들어
//      클라이언트 역할을 시켰더니, 그 두 번째 스레드의 두 번째
//      syscall(SocketConnect)부터 커널 핸들러가 아예 호출 안 됨.
//   2) 이 프로세스가 SpawnProcess로 sockclient를 직접 낳고 그 직후
//      자신의 다음 syscall(Accept)을 제출했더니, **이번엔 이
//      프로세스 자신의 Accept() 핸들러가 호출 안 됨**(sockclient
//      쪽은 Connect()까지 정상 진입) - dbgdriver가 SpawnProcess를
//      쓸 때는 항상 `kSpawnDebugStart`로 자식을 정지시킨 채 낳아서
//      "낳은 직후 자식이 진짜로 동시에 실행되는" 경우가 이 프로젝트
//      최초였다.
// 두 경우 다 공통점은 "두 Task가 진짜로 동시에(SMP) syscall을
// 주고받는" 조합이라는 것 - 이 프로젝트가 이미 여러 세션째 쫓고
// 있는 AsyncTask 완료 통지 유실류 heisenbug(PN-6360E6E9/PN-D44504D1)
// 와 같은 계열일 가능성이 높아 별도 계획(PN-395F4D89)으로 분리
// 추적하고, 이 검증 자체는 이 프로젝트가 부팅 때부터 수백 번
// 검증해 온 "커널이 여러 프로세스를 각자 독립적으로 스폰, 서로
// Channel/named object로 찾아 연결"(init 없이도 pubreg+authmgr가
// 항상 이렇게 공존) 패턴으로 우회한다 - 유일한 차이는 Bind()가
// 먼저 끝나길 보장할 방법이 없어(SignalUserlandReady류 조정 채널이
// 이 둘 사이엔 없음) sockclient가 Connect()를 몇 차례 재시도한다
// (sockclient/main.cpp 참고).
//
// **실행하려면 TEMP 부팅 훅이 필요하다** - dbgdriver와 동일한 이유
// (부팅 매니페스트에는 없음, kmain.cpp에 이름 "socktest"/"sockclient"
// 를 매치하는 전용 스폰 경로를 임시로 추가) - 조사가 끝나면 그
// 훅들은 되돌린다(이 파일/sockclient 자체와 CMake/build-initrd.sh
// 배선은 dbgtarget/proctest와 동일하게 영구 유지).
//
// exitCode 규약(mc::selfTerminate의 exitCode 자체는 v1에서 커널이 안
// 씀 - 실제 확인은 kmain.cpp/vfs_syscall.cpp/socket.cpp에 TEMP
// Logger::info를 심어서 한다):
//   0 = 전 구간 정상 왕복 성공
//   1 = Socket(A) 실패
//   2 = Bind(A) 실패
//   3 = Listen(A) 실패
//   4 = Accept(A) 실패
//   5 = Read(accepted) 실패
//   6 = Read된 바이트 수 불일치
//   7 = Read된 내용 불일치
#include "libmc/socket.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kSockName[] = "socktest.sock";
constexpr mc::uint32_t kSockNameLen = sizeof(kSockName) - 1;
constexpr char kMsg[] = "hello-socket";
constexpr mc::uint32_t kMsgLen = sizeof(kMsg) - 1;

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

}  // namespace

extern "C" void _start() {
    // 1) Socket(A) - Stream, 리스너.
    mc::SocketArgs sockA;
    sockA.domain = mc::SocketDomain::Unix;
    sockA.type = mc::SocketType::Stream;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSocket, &sockA);
    if (token == 0 || !mc::wait(token) || sockA.error != mc::ChannelError::None) {
        kFinish(1);
    }

    // 2) Bind(A, "socktest.sock").
    mc::SocketBindArgs bindArgs;
    bindArgs.fd = static_cast<mc::int32_t>(sockA.fd);
    bindArgs.path = kSockName;
    bindArgs.pathLen = kSockNameLen;
    token = mc::submit(mc::kSyscallEndpointSocketBind, &bindArgs);
    if (token == 0 || !mc::wait(token) || bindArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }

    // 3) Listen(A).
    mc::SocketListenArgs listenArgs;
    listenArgs.fd = static_cast<mc::int32_t>(sockA.fd);
    listenArgs.backlog = 1;
    token = mc::submit(mc::kSyscallEndpointSocketListen, &listenArgs);
    if (token == 0 || !mc::wait(token) || listenArgs.error != mc::ChannelError::None) {
        kFinish(3);
    }

    // 4) Accept(A) - sockclient(독립 프로세스, kmain.cpp가 이 프로세스와
    //    거의 동시에 스폰)가 보낼 connect를 소비한다(연결 대기까지
    //    블로킹).
    mc::SocketAcceptArgs acceptArgs;
    acceptArgs.fd = static_cast<mc::int32_t>(sockA.fd);
    token = mc::submit(mc::kSyscallEndpointSocketAccept, &acceptArgs);
    if (token == 0 || !mc::wait(token) || acceptArgs.error != mc::ChannelError::None) {
        kFinish(4);
    }

    // 5) Read(accepted) - 소켓 fd가 기존 Read(그룹3) 그대로 동작하는지,
    //    sockclient가 쓴 바이트가 그대로 도착하는지 확인.
    static mc::uint8_t buf[64];
    mc::ReadArgs readArgs;
    readArgs.fd = static_cast<mc::int32_t>(acceptArgs.newFd);
    readArgs.buf = buf;
    readArgs.len = sizeof(buf);
    token = mc::submit(mc::kSyscallEndpointRead, &readArgs);
    if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None) {
        kFinish(5);
    }
    if (readArgs.bytesRead != kMsgLen) {
        kFinish(6);
    }
    for (mc::uint32_t i = 0; i < kMsgLen; ++i) {
        if (buf[i] != static_cast<mc::uint8_t>(kMsg[i])) {
            kFinish(7);
        }
    }

    // 6) 정리 - Close(그룹3)가 리스너/연결 fd 둘 다 정상 반납하는지.
    mc::CloseArgs closeArgs;
    closeArgs.fd = static_cast<mc::int32_t>(sockA.fd);
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    if (token != 0) {
        mc::wait(token);
    }
    closeArgs.fd = static_cast<mc::int32_t>(acceptArgs.newFd);
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    if (token != 0) {
        mc::wait(token);
    }

    kFinish(0);
}
