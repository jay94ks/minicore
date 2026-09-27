// minicore/sockinherit - PN-CC0F4EAC 항목7(fd 상속/LISTEN_FDS) 전용
// 최소 실측 검증 프로그램(활성화자/"activator" 역할) -
// minicore/sockinheritchild(fd를 물려받는 쪽)와 짝을 이룬다. 서로 다른
// 프로세스지만 이번엔 socktest/sockclient와 달리 **진짜 SpawnProcess로
// 부모가 자식을 낳는다** - PN-395F4D89(두 Task가 진짜 동시에 syscall을
// 주고받을 때의 heisenbug)를 피하려고, 자식을 kSpawnDebugStart로 정지된
// 채 스폰하지는 않지만 부모가 spawn 직후 그 자리에서 바로 자기 자신의
// 다음 syscall(Close)을 제출하는 "동시성 있는 다음 syscall" 패턴 자체는
// dbgdriver가 이미 실측 검증해 둔 안전한 조합(자식이 kSpawnDebugStart로
// 정지 상태로 시작해 실제로는 동시에 돌지 않음)과 다르다 - 이 프로그램은
// 의도적으로 그 위험한 조합을 다시 밟는다(자식이 즉시 실행됨) - 이것도
// 하나의 실측 데이터 포인트로 남긴다(정직하게 기록).
//
// **실행하려면 TEMP 부팅 훅이 필요하다** - dbgdriver/socktest와 동일한
// 이유(부팅 매니페스트에는 없음, kmain.cpp에 이름 "sockinherit"/
// "sockinheritchild"를 매치하는 전용 스폰 경로를 임시로 추가) - 조사가
// 끝나면 그 훅들은 되돌린다(이 파일/sockinheritchild 자체와 CMake/
// build-initrd.sh 배선은 dbgtarget/proctest와 동일하게 영구 유지).
//
// 시나리오: Socket(Stream)→Bind("socktest.sock")→Listen()로 리스너
// fd(=0으로 기대)를 준비한 뒤, minicore/sockinheritchild를 initrd에서
// 읽어 SpawnProcess(flags=kSpawnInheritFds)로 스폰한다 - 자식이 이
// fd=0을 그대로 물려받아 Accept()할 것을 기대한다(이 fd 번호 가정은
// 자식이 스폰 시점에 fd 테이블이 비어 있고 이 부모가 물려주는 소켓이
// 유일한 상속 대상이라는 이 테스트 고유의 전제 - 실사용 프로그램은
// LISTEN_PID/LISTEN_FDS envp를 실제로 파싱해야 하지만, 이 커널의
// 유저랜드 런타임엔 아직 envp 파싱 인프라 자체가 없어(어떤 기존
// 프로그램도 argv/envp를 읽지 않음) 이 하드코딩으로 우회한다 - 커널
// 쪽 LISTEN_PID/LISTEN_FDS 계산 자체는 process.cpp에 TEMP 로그를 심어
// 별도로 검증한다). 그 다음 **자신의 fd=0 사본을 즉시 Close()**해
// useCount()가 2(부모+자식)→1(자식만)로 내려가는 경로를 실측한다 -
// CloseHandler가 이 시점에 실제로 Channel/Bridge를 파괴하지 않아야
// 자식이 여전히 정상 Accept()할 수 있다.
//
// exitCode 규약(TEMP Logger::info로 교차 확인, mc::selfTerminate의
// exitCode 자체는 v1에서 커널이 안 씀):
//   0 = 전 구간 정상(Socket/Bind/Listen/SpawnProcess/Close 전부 성공)
//   1 = Socket 실패
//   2 = Bind 실패
//   3 = Listen 실패
//   4 = /sys/live/initrd.cpio Stat/Open/Read/Close 실패
//   5 = cpio 아카이브 안에서 "sockinheritchild" 엔트리를 못 찾음
//   6 = SpawnProcess 실패
//   7 = 자기 자신의 fd Close 실패
#include "libcpio/cpio.h"
#include "libmc/process.h"
#include "libmc/signal.h"
#include "libmc/socket.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kSockName[] = "socktest.sock";
constexpr mc::uint32_t kSockNameLen = sizeof(kSockName) - 1;

constexpr mc::uint64_t kMaxInitrdSize = 4 * 1024 * 1024;
mc::uint8_t gInitrdBuffer[kMaxInitrdSize];

struct FindChildContext {
    const void* data = nullptr;
    mc::uint64_t dataSize = 0;
    bool found = false;
};

void kFindChildEntry(const cpio::Entry& entry, void* userData) {
    auto* ctx = static_cast<FindChildContext*>(userData);
    if (ctx->found) {
        return;
    }
    constexpr char kName[] = "sockinheritchild";
    constexpr mc::uint32_t kNameLen = sizeof(kName) - 1;
    if (entry.nameLength != kNameLen) {
        return;
    }
    for (mc::uint32_t i = 0; i < kNameLen; ++i) {
        if (entry.name[i] != kName[i]) {
            return;
        }
    }
    ctx->data = entry.data;
    ctx->dataSize = entry.dataSize;
    ctx->found = true;
}

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

}  // namespace

extern "C" void _start() {
    // 0) [수정, 2026-09-27] SIGCHLD를 Ignore로 설정 - PN-012D6310가 이미
    // 문서화해 둔 전제(libmc/signal.h SignalNumber::Chld 문서 주석
    // 참고)를 이 프로그램이 놓치고 있었다: SpawnProcess로 자식을 낳는
    // 프로세스는 자식이 죽을 때(sockinheritchild가 정상 종료하는 것도
    // 포함) 자기 자신도 함께 죽지 않으려면 SIGCHLD를 명시적으로
    // Ignore로 설정해야 한다(기본 disposition은 종료) - 이걸 빠뜨려서
    // 이 프로그램이 100% 재현되는 "SIGSEGV처럼 보이는" 조기 종료를
    // 겪고 있었다(실제로는 SIGSEGV가 아니라 SIGCHLD 기본 처리, 실측
    // 확인 완료 - PN-CC0F4EAC 참고).
    mc::SignalActionArgs sigArgs;
    sigArgs.signal = mc::SignalNumber::Chld;
    sigArgs.disposition = mc::SignalDisposition::Ignore;
    mc::SyscallToken sigToken = mc::submit(mc::kSyscallEndpointSignalAction, &sigArgs);
    if (sigToken != 0) {
        mc::wait(sigToken);
    }

    // 1) Socket(Stream) - 리스너.
    mc::SocketArgs sockArgs;
    sockArgs.domain = mc::SocketDomain::Unix;
    sockArgs.type = mc::SocketType::Stream;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSocket, &sockArgs);
    if (token == 0 || !mc::wait(token) || sockArgs.error != mc::ChannelError::None) {
        kFinish(1);
    }

    // 2) Bind(fd, "socktest.sock") - sockclient가 그대로 찾아온다.
    mc::SocketBindArgs bindArgs;
    bindArgs.fd = static_cast<mc::int32_t>(sockArgs.fd);
    bindArgs.path = kSockName;
    bindArgs.pathLen = kSockNameLen;
    token = mc::submit(mc::kSyscallEndpointSocketBind, &bindArgs);
    if (token == 0 || !mc::wait(token) || bindArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }

    // 3) Listen(fd).
    mc::SocketListenArgs listenArgs;
    listenArgs.fd = static_cast<mc::int32_t>(sockArgs.fd);
    listenArgs.backlog = 1;
    token = mc::submit(mc::kSyscallEndpointSocketListen, &listenArgs);
    if (token == 0 || !mc::wait(token) || listenArgs.error != mc::ChannelError::None) {
        kFinish(3);
    }

    // 4) /sys/live/initrd.cpio에서 sockinheritchild ELF 읽기(dbgdriver/
    //    main.cpp와 동일한 관례).
    constexpr char kInitrdPath[] = "/sys/live/initrd.cpio";
    constexpr mc::uint32_t kInitrdPathLen = sizeof(kInitrdPath) - 1;

    mc::StatArgs statArgs;
    statArgs.path = kInitrdPath;
    statArgs.pathLen = kInitrdPathLen;
    token = mc::submit(mc::kSyscallEndpointStat, &statArgs);
    if (token == 0 || !mc::wait(token) || statArgs.error != mc::ChannelError::None) {
        kFinish(4);
    }
    if (statArgs.size > kMaxInitrdSize) {
        kFinish(4);
    }

    mc::OpenArgs openArgs;
    openArgs.path = kInitrdPath;
    openArgs.pathLen = kInitrdPathLen;
    openArgs.flags = static_cast<mc::uint32_t>(mc::OpenFlags::ReadOnly);
    token = mc::submit(mc::kSyscallEndpointOpen, &openArgs);
    if (token == 0 || !mc::wait(token) || openArgs.error != mc::ChannelError::None) {
        kFinish(4);
    }

    mc::uint64_t totalRead = 0;
    while (totalRead < statArgs.size) {
        mc::ReadArgs readArgs;
        readArgs.fd = openArgs.fd;
        readArgs.buf = gInitrdBuffer + totalRead;
        readArgs.len = static_cast<mc::uint32_t>(statArgs.size - totalRead);
        token = mc::submit(mc::kSyscallEndpointRead, &readArgs);
        if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None) {
            kFinish(4);
        }
        if (readArgs.bytesRead == 0) {
            break;
        }
        totalRead += readArgs.bytesRead;
    }

    mc::CloseArgs initrdCloseArgs;
    initrdCloseArgs.fd = openArgs.fd;
    token = mc::submit(mc::kSyscallEndpointClose, &initrdCloseArgs);
    if (token != 0) {
        mc::wait(token);
    }

    if (totalRead != statArgs.size) {
        kFinish(4);
    }

    FindChildContext findCtx;
    cpio::forEachEntry(gInitrdBuffer, totalRead, kFindChildEntry, &findCtx);
    if (!findCtx.found) {
        kFinish(5);
    }

    // 5) SpawnProcess(flags=kSpawnInheritFds) - 자식이 위 리스너 소켓
    //    fd를 그대로 물려받는다.
    mc::SpawnProcessArgs spawnArgs;
    spawnArgs.imageBuffer = findCtx.data;
    spawnArgs.imageSize = findCtx.dataSize;
    spawnArgs.flags = mc::SpawnProcessFlags::kSpawnInheritFds;
    token = mc::submit(mc::kSyscallEndpointSpawnProcess, &spawnArgs);
    if (token == 0 || !mc::wait(token) || spawnArgs.error != mc::SpawnProcessError::None) {
        kFinish(6);
    }

    // 6) 자기 자신의 리스너 fd 사본을 즉시 Close() - useCount()가
    //    2(부모+자식)→1(자식만)로 내려가는 경로를 실측한다. 자식이
    //    여전히 정상 Accept()할 수 있어야 한다(=이 Close()가 공유
    //    자원을 파괴하지 않았다는 뜻).
    mc::CloseArgs closeArgs;
    closeArgs.fd = static_cast<mc::int32_t>(sockArgs.fd);
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    if (token == 0 || !mc::wait(token) || closeArgs.error != mc::ChannelError::None) {
        kFinish(7);
    }

    kFinish(0);
}
