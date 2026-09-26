// minicore/sockinheritchild - minicore/sockinherit(PN-CC0F4EAC 항목7
// fd 상속 실측 검증)의 자식 반쪽 - sockinherit/main.cpp 상단 주석 참고.
// 진짜 SpawnProcess(flags=kSpawnInheritFds)로 낳아진 자식으로서, 부모가
// Bind+Listen까지 끝내 둔 소켓 fd를 그대로 물려받아 fd=0으로 이어받았을
// 것을 가정한다(이 가정의 근거는 sockinherit/main.cpp 문서 주석 참고 -
// 이 커널 유저랜드 런타임에 아직 envp 파싱 인프라가 없어 LISTEN_PID/
// LISTEN_FDS를 실제로 읽지 못하고 하드코딩으로 우회한다).
//
// minicore/sockclient(기존 영구 검증 프로그램, "socktest.sock"에 연결해
// "hello-socket"을 쓰고 닫는다)를 그대로 재사용해 이 fd=0으로 들어오는
// 연결을 만든다 - socktest 대신 이 프로그램이 그 역할을 대신할 뿐 클라
// 이언트 쪽 프로토콜은 완전히 동일하다.
//
// exitCode 규약(TEMP Logger::info로 교차 확인):
//   0 = 전 구간 정상(Accept/Read/내용 일치/Close 전부 성공)
//   1 = Accept(fd=0) 실패
//   2 = Read(accepted) 실패
//   3 = Read된 바이트 수 불일치
//   4 = Read된 내용 불일치
//   5 = Close 실패(accepted 또는 fd=0)
#include "libmc/socket.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kMsg[] = "hello-socket";
constexpr mc::uint32_t kMsgLen = sizeof(kMsg) - 1;
constexpr mc::int32_t kInheritedFd = 0;

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

}  // namespace

extern "C" void _start() {
    // 1) Accept(fd=0) - 물려받은 리스너 소켓에 sockclient의 connect를
    //    기다린다(블로킹). 부모(sockinherit)가 자기 fd 사본을 이미
    //    Close()했더라도(useCount() 1로 하향) 이 호출이 정상 동작해야
    //    한다 - 그게 바로 이 검증의 핵심 단언이다.
    mc::SocketAcceptArgs acceptArgs;
    acceptArgs.fd = kInheritedFd;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSocketAccept, &acceptArgs);
    if (token == 0 || !mc::wait(token) || acceptArgs.error != mc::ChannelError::None) {
        kFinish(1);
    }

    // 2) Read(accepted) - 기존 Read(그룹3)가 소켓 fd에도 그대로 동작.
    static mc::uint8_t buf[64];
    mc::ReadArgs readArgs;
    readArgs.fd = static_cast<mc::int32_t>(acceptArgs.newFd);
    readArgs.buf = buf;
    readArgs.len = sizeof(buf);
    token = mc::submit(mc::kSyscallEndpointRead, &readArgs);
    if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }
    if (readArgs.bytesRead != kMsgLen) {
        kFinish(3);
    }
    for (mc::uint32_t i = 0; i < kMsgLen; ++i) {
        if (buf[i] != static_cast<mc::uint8_t>(kMsg[i])) {
            kFinish(4);
        }
    }

    // 3) 정리 - accepted fd + 물려받은 리스너 fd 둘 다 닫는다. 이
    //    리스너 fd Close()는 이 시점에 useCount()==1(부모가 이미
    //    내려놓았으므로)이라 실제로 Channel/Bridge/NamedObjectTable
    //    이름까지 전부 파괴해야 한다 - 이번 세션 검증의 두 번째 핵심
    //    단언(마지막 소유자 경로가 여전히 정상 동작하는지).
    mc::CloseArgs closeArgs;
    closeArgs.fd = static_cast<mc::int32_t>(acceptArgs.newFd);
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    bool ok = (token != 0) && mc::wait(token) && closeArgs.error == mc::ChannelError::None;

    closeArgs = mc::CloseArgs{};
    closeArgs.fd = kInheritedFd;
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    ok = ok && (token != 0) && mc::wait(token) && closeArgs.error == mc::ChannelError::None;

    if (!ok) {
        kFinish(5);
    }

    kFinish(0);
}
