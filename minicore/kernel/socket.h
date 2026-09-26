#ifndef MINICORE_KERNEL_SOCKET_H
#define MINICORE_KERNEL_SOCKET_H

#include "channel.h"
#include "libkenv/types.h"
#include "named_object.h"
#include "syscall.h"

namespace kernel {

// 소켓 계층(SP-231493CB) - v1은 AF_UNIX(로컬 소켓)만 다룬다(§2, 실제
// 네트워킹은 NIC 드라이버가 전혀 없어 범위 밖). 핵심 설계 결정(§3):
// 소켓은 새 IPC 프리미티브가 아니라 **`Channel`의 POSIX 호환
// 파사드**다 - 이 파일의 syscall 핸들러(socket.cpp)는 실제 랑데부/
// 링버퍼 로직을 전혀 새로 구현하지 않고, 이미 있는 Channel 계열
// syscall 핸들러(channel.h의 OpenChannel/ConnectChannel/
// AcceptFromChannel/ChannelRead/ChannelWrite/CloseBridge)에 내부적으로
// AsyncTask를 다시 제출해(vfs_syscall.cpp의 StatHandler가 KernelFsDriver
// 에 재제출하는 것과 동일한 합성 패턴) 위임한다.
enum class SocketDomain : uint32_t {
    Unix = 1,  // AF_INET 등은 후속(§7) - NIC 드라이버가 생기기 전엔 정의하지 않는다(RM-23F4B687 §4)
};

enum class SocketType : uint32_t {
    Stream = 1,
    Datagram = 2,
};

// UnixSocket - `Process::FileDescriptor::socket`(kind==MountKind::Socket
// 일 때만 유효)가 GenericSlabAllocator로 확보해 단독 소유하는 상태
// 블록. BridgePipe/Channel과 달리 SharedPtr로 감싸지 않는다 - 지금은
// 이 fd 하나만이 유일한 소유자이기 때문(fd 상속(§6, PN-CC0F4EAC 항목7)
// 이 나중에 "여러 fd가 같은 소켓을 공유"하는 경우를 실제로 만들면
// 그때 SharedPtr로 승격 - RM-23F4B687 §4, 지금은 실사용처가 없다).
struct UnixSocket {
    SocketType type = SocketType::Stream;

    // `Socket()`이 항상 만드는(익명) Channel의 ChannelId - §4-1 자동
    // 등록 경로(NamedObjectTable)가 가리키는 실체다. `Accept()`가
    // 만들어내는(이미 연결된) 소켓은 자기 자신의 Channel이 없으므로
    // 0 - 그 자신을 다시 accept()할 대상이 아니기 때문(§4-1이 말하는
    // "Socket()으로 만들어지는 모든 소켓"에 accept() 산출물은 포함되지
    // 않는다는 뜻과 동일 - 새로 연결이 열릴 때마다 이름공간에 의미
    // 없는 항목이 계속 쌓이는 것을 막는다).
    ChannelId channelId = 0;

    // `Connect()`(클라이언트) 또는 `Accept()`(서버)가 성공한 뒤에만
    // 유효(0이면 아직 미연결) - 이후 이 fd에 대한 Read/Write/Close
    // (그룹3, vfs_syscall.cpp)는 전부 이 값으로 기존 ChannelRead/
    // ChannelWrite/CloseBridge를 그대로 위임한다(§3).
    BridgeHandle bridge = 0;

    // `Listen()`이 설정 - **[정직하게 기록] backlog는 정보 제공용일
    // 뿐 강제되지 않는다** - 밑바탕 Channel의 accept 큐(AsyncTaskWaitQueue,
    // PN-C9625015)는 이미 무제한 다중 pending connect/accepter를
    // 지원해서 별도 상한을 걸 필요가 없었다(실제 소비자가 생겨 정책이
    // 필요해지면 그때 강제 - RM-23F4B687 §4).
    bool listening = false;
    uint32_t backlog = 0;

    // [신규] `Bind()`(§4-2 항목1)로 추가 등록한 이름 - v1은 소켓당
    // 최대 1회만 허용(POSIX의 "이미 bind된 소켓 재bind 금지" 관례와
    // 동일, 실사용 근거 없이 다중 이름을 허용하지 않는다). §4-1 자동
    // 등록 경로와 별도로 기억해 둬야 `Close()`가 이 이름도 반납할 수
    // 있다(안 그러면 소켓이 죽은 뒤에도 NamedObjectTable에 이름만
    // 영구히 남는 네임스페이스 누수).
    bool explicitlyBound = false;
    char boundPath[kMaxNamedObjectNameLength] = {};
    uint32_t boundPathLength = 0;
};

// §4-1 자동 등록 경로 포맷("<pid>/<handle>") - `Socket()`(socket.cpp,
// 등록)과 `CloseHandler`(vfs_syscall.cpp, 해제)가 정확히 같은 포맷을
// 써야 이름이 어긋나지 않는다 - 이 프로젝트의 "작은 헬퍼는 파일마다
// 따로 구현"관례의 예외(그 경우들과 달리 여기선 두 구현이 갈리면
// 바로 정합성 버그가 된다).
uint32_t kFormatAutoSocketPath(int64_t pid, int32_t fd, char* buf, uint32_t bufCap);

// syscall 그룹 11(RM-48E1E610, "번호만 예약"으로 이미 등록돼 있던 표
// 그대로) - Send/Recv 전용 syscall은 없다(§5, 기존 Read/Write 그룹3을
// 그대로 쓴다).
constexpr SyscallEndpointId kSyscallEndpointSocket = kMakeSyscallEndpointId(11, 0);
constexpr SyscallEndpointId kSyscallEndpointSocketBind = kMakeSyscallEndpointId(11, 1);
constexpr SyscallEndpointId kSyscallEndpointSocketListen = kMakeSyscallEndpointId(11, 2);
constexpr SyscallEndpointId kSyscallEndpointSocketAccept = kMakeSyscallEndpointId(11, 3);
constexpr SyscallEndpointId kSyscallEndpointSocketConnect = kMakeSyscallEndpointId(11, 4);
// [예약, 미구현, 2026-09-27] Shutdown - 밑바탕 Channel 계층에 "핸들은
// 열어 둔 채 한쪽 방향만 닫기"에 대응하는 기존 프리미티브가 없다
// (`CloseBridge`/`kCloseBridgeSync`는 항상 완전히 닫고 그 자리에서
// 호출자의 openBridges에서 바로 제거한다, channel.cpp 참고) - 새
// Channel 프리미티브 설계가 필요해 이번 증분 범위 밖(PN-CC0F4EAC
// 후속으로 명시적으로 남겨 둠).
constexpr SyscallEndpointId kSyscallEndpointSocketShutdown = kMakeSyscallEndpointId(11, 5);

struct SocketArgs {
    SocketDomain domain = SocketDomain::Unix;
    SocketType type = SocketType::Stream;
    // out
    ChannelError error = ChannelError::None;
    int64_t fd = -1;
};

struct SocketBindArgs {
    int32_t fd = -1;
    const char* path = nullptr;  // in: 유저 메모리. v1은 '/' 없는 단순 이름만(NamedObjectTable) -
                                  // '/'로 시작하는 실제 VFS 경로 바인드(§4-2 항목2/3)는 이번 증분 범위 밖.
    uint32_t pathLen = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct SocketListenArgs {
    int32_t fd = -1;
    uint32_t backlog = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct SocketAcceptArgs {
    int32_t fd = -1;  // Stream 전용 - 연결 대기까지 블로킹(§5)
    // out
    ChannelError error = ChannelError::None;
    int64_t newFd = -1;
};

struct SocketConnectArgs {
    int32_t fd = -1;
    const char* path = nullptr;  // in: 유저 메모리, SocketBindArgs::path와 동일한 v1 제약(단순 이름만)
    uint32_t pathLen = 0;
    // out
    ChannelError error = ChannelError::None;
};

class Socket {
public:
    // 부팅 시 한 번 호출 - 위 5개 endpoint(Shutdown 제외, 아직 미구현)를
    // SyscallRegistry에 등록한다.
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SOCKET_H
