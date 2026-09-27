// minicore/epolltest - SP-6350DEBB(epoll류 유저 영역 이벤트 다중화)/
// PN-7562DA62 전용 최소 실측 검증 프로그램(서버 역할) - 기존
// minicore/sockclient(클라이언트 역할, "socktest.sock" 이름 하드코딩)
// 를 그대로 재사용해 짝을 이룬다 - socktest/main.cpp가 이미 확립한
// "완전히 독립된 프로세스 둘이 이름으로 서로를 찾아 연결"(SMP 동시
// syscall heisenbug 우회) 패턴을 그대로 따른다.
//
// **socktest와 같은 부팅에 절대 함께 켜지 않는다** - 둘 다 리슨
// 소켓을 sockclient와 같은 이름("socktest.sock")으로 bind해야 해서,
// 함께 켜면 NamedObjectTable 이름 경합이 생긴다(이 세션이 이미 겪은
// 함정, PN-6360E6E9 정정 절 참고). **실행하려면 TEMP 부팅 훅이
// 필요하다** - dbgdriver/socktest와 동일한 이유(부팅 매니페스트에는
// 없음). 조사가 끝나면 그 훅은 되돌린다(이 파일 자체와 CMake/
// build-initrd.sh 배선은 dbgtarget/proctest와 동일하게 영구 유지).
//
// exitCode 규약:
//   0 = 전 구간 정상 성공
//   1 = Socket(리스너) 실패
//   2 = Bind 실패
//   3 = Listen 실패
//   4 = EpollCreate 실패
//   5 = EpollCtl(Add, 리슨fd, Readable) 실패
//   6 = EpollWait(접속 대기) 실패/타임아웃/잘못된 결과(§8-1 검증)
//   7 = Accept 실패(epoll이 준비됐다고 보고한 직후)
//   8 = EpollCtl(Add, 연결된fd, Readable) 실패
//   9 = EpollWait(데이터 대기) 실패/타임아웃/잘못된 결과
//  10 = Read 실패(epoll이 readable이라고 보고한 직후)
//  11 = Read된 바이트 수/내용 불일치
//  12 = [§8-2 레벨 트리거 오탐 검증] 아무 일도 없는데 EpollWait이
//       짧은 타임아웃 안에 이벤트를 보고함(false positive)
//  13 = EpollCtl(Del) 실패
#include "libmc/epoll.h"
#include "libmc/socket.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kSockName[] = "socktest.sock";  // sockclient 하드코딩과 반드시 일치
constexpr mc::uint32_t kSockNameLen = sizeof(kSockName) - 1;
constexpr char kMsg[] = "hello-socket";
constexpr mc::uint32_t kMsgLen = sizeof(kMsg) - 1;
constexpr mc::uint64_t kAcceptUserData = 0xA11CE;
constexpr mc::uint64_t kReadUserData = 0xB0B;

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

}  // namespace

extern "C" void _start() {
    // 1) Socket(리스너) - Stream.
    mc::SocketArgs listenSock;
    listenSock.domain = mc::SocketDomain::Unix;
    listenSock.type = mc::SocketType::Stream;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSocket, &listenSock);
    if (token == 0 || !mc::wait(token) || listenSock.error != mc::ChannelError::None) {
        kFinish(1);
    }

    // 2) Bind - sockclient가 하드코딩한 바로 그 이름.
    mc::SocketBindArgs bindArgs;
    bindArgs.fd = static_cast<mc::int32_t>(listenSock.fd);
    bindArgs.path = kSockName;
    bindArgs.pathLen = kSockNameLen;
    token = mc::submit(mc::kSyscallEndpointSocketBind, &bindArgs);
    if (token == 0 || !mc::wait(token) || bindArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }

    // 3) Listen.
    mc::SocketListenArgs listenArgs;
    listenArgs.fd = static_cast<mc::int32_t>(listenSock.fd);
    listenArgs.backlog = 1;
    token = mc::submit(mc::kSyscallEndpointSocketListen, &listenArgs);
    if (token == 0 || !mc::wait(token) || listenArgs.error != mc::ChannelError::None) {
        kFinish(3);
    }

    // 4) EpollCreate.
    mc::EpollCreateArgs createArgs;
    token = mc::submit(mc::kSyscallEndpointEpollCreate, &createArgs);
    if (token == 0 || !mc::wait(token) || createArgs.error != mc::ChannelError::None) {
        kFinish(4);
    }
    const mc::int32_t epfd = static_cast<mc::int32_t>(createArgs.fd);

    // 5) EpollCtl(Add, 리슨fd, Readable) - "접속 가능"을 감시.
    mc::EpollCtlArgs ctlArgs;
    ctlArgs.epfd = epfd;
    ctlArgs.op = mc::EpollCtlOp::Add;
    ctlArgs.targetFd = static_cast<mc::int32_t>(listenSock.fd);
    ctlArgs.mask = static_cast<mc::uint32_t>(mc::EpollEventMask::Readable);
    ctlArgs.userData = kAcceptUserData;
    token = mc::submit(mc::kSyscallEndpointEpollCtl, &ctlArgs);
    if (token == 0 || !mc::wait(token) || ctlArgs.error != mc::ChannelError::None) {
        kFinish(5);
    }

    // 6) EpollWait - sockclient(독립 프로세스, kmain.cpp가 이 프로세스와
    //    거의 동시에 스폰)가 연결을 시도할 때까지 블로킹(busy-poll이
    //    아니라 실제 완료 통지로 깨어나는지가 이 검증의 핵심).
    mc::EpollReadyEvent events[4];
    mc::EpollWaitArgs waitArgs;
    waitArgs.epfd = epfd;
    waitArgs.outEvents = events;
    waitArgs.maxEvents = 4;
    waitArgs.timeoutMs = 15000;
    token = mc::submit(mc::kSyscallEndpointEpollWait, &waitArgs);
    if (token == 0 || !mc::wait(token) || waitArgs.error != mc::ChannelError::None) {
        kFinish(6);
    }
    if (waitArgs.count != 1 || events[0].userData != kAcceptUserData ||
        (events[0].events & static_cast<mc::uint32_t>(mc::EpollEventMask::Readable)) == 0) {
        kFinish(6);
    }

    // 7) Accept - epoll이 "준비됐다"고 알려준 뒤에야 부른다(진짜
    //    epoll_wait -> accept 관용구).
    mc::SocketAcceptArgs acceptArgs;
    acceptArgs.fd = static_cast<mc::int32_t>(listenSock.fd);
    token = mc::submit(mc::kSyscallEndpointSocketAccept, &acceptArgs);
    if (token == 0 || !mc::wait(token) || acceptArgs.error != mc::ChannelError::None) {
        kFinish(7);
    }

    // 8) EpollCtl(Add, 연결된fd, Readable) - 이번엔 데이터 도착을 감시.
    ctlArgs = mc::EpollCtlArgs{};
    ctlArgs.epfd = epfd;
    ctlArgs.op = mc::EpollCtlOp::Add;
    ctlArgs.targetFd = static_cast<mc::int32_t>(acceptArgs.newFd);
    ctlArgs.mask = static_cast<mc::uint32_t>(mc::EpollEventMask::Readable);
    ctlArgs.userData = kReadUserData;
    token = mc::submit(mc::kSyscallEndpointEpollCtl, &ctlArgs);
    if (token == 0 || !mc::wait(token) || ctlArgs.error != mc::ChannelError::None) {
        kFinish(8);
    }

    // 9) EpollWait - sockclient가 Write()할 때까지 블로킹.
    waitArgs = mc::EpollWaitArgs{};
    waitArgs.epfd = epfd;
    waitArgs.outEvents = events;
    waitArgs.maxEvents = 4;
    waitArgs.timeoutMs = 15000;
    token = mc::submit(mc::kSyscallEndpointEpollWait, &waitArgs);
    if (token == 0 || !mc::wait(token) || waitArgs.error != mc::ChannelError::None) {
        kFinish(9);
    }
    if (waitArgs.count != 1 || events[0].userData != kReadUserData ||
        (events[0].events & static_cast<mc::uint32_t>(mc::EpollEventMask::Readable)) == 0) {
        kFinish(9);
    }

    // 10) Read - epoll이 readable이라고 보고한 직후.
    static mc::uint8_t buf[64];
    mc::ReadArgs readArgs;
    readArgs.fd = static_cast<mc::int32_t>(acceptArgs.newFd);
    readArgs.buf = buf;
    readArgs.len = sizeof(buf);
    token = mc::submit(mc::kSyscallEndpointRead, &readArgs);
    if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None) {
        kFinish(10);
    }
    if (readArgs.bytesRead != kMsgLen) {
        kFinish(11);
    }
    for (mc::uint32_t i = 0; i < kMsgLen; ++i) {
        if (buf[i] != static_cast<mc::uint8_t>(kMsg[i])) {
            kFinish(11);
        }
    }

    // 11) [§8-2 레벨 트리거 오탐 검증] 방금 데이터를 전부 읽어 큐가
    //     비었고, sockclient는 더 이상 아무것도 보내지 않는다(곧 종료).
    //     짧은 타임아웃 안에 이 fd가 다시 "준비됨"으로 잘못 보고되면
    //     안 된다(진짜 이벤트가 없는데 폴링처럼 계속 깨어나면 레벨
    //     트리거 판정 로직이 근본적으로 잘못된 것).
    waitArgs = mc::EpollWaitArgs{};
    waitArgs.epfd = epfd;
    waitArgs.outEvents = events;
    waitArgs.maxEvents = 4;
    waitArgs.timeoutMs = 300;
    token = mc::submit(mc::kSyscallEndpointEpollWait, &waitArgs);
    if (token == 0 || !mc::wait(token) || waitArgs.error != mc::ChannelError::None) {
        kFinish(12);
    }
    if (waitArgs.count != 0) {
        kFinish(12);
    }

    // 12) 정리.
    ctlArgs = mc::EpollCtlArgs{};
    ctlArgs.epfd = epfd;
    ctlArgs.op = mc::EpollCtlOp::Del;
    ctlArgs.targetFd = static_cast<mc::int32_t>(acceptArgs.newFd);
    token = mc::submit(mc::kSyscallEndpointEpollCtl, &ctlArgs);
    if (token == 0 || !mc::wait(token) || ctlArgs.error != mc::ChannelError::None) {
        kFinish(13);
    }

    mc::CloseArgs closeArgs;
    closeArgs.fd = static_cast<mc::int32_t>(listenSock.fd);
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    if (token != 0) {
        mc::wait(token);
    }
    closeArgs.fd = static_cast<mc::int32_t>(acceptArgs.newFd);
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    if (token != 0) {
        mc::wait(token);
    }
    closeArgs.fd = epfd;
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    if (token != 0) {
        mc::wait(token);
    }

    kFinish(0);
}
