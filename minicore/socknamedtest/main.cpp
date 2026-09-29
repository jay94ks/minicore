// minicore/socknamedtest - PN-F9CBF1A9(SP-231493CB §4-1 후속, 소켓
// 자동 등록 경로를 NamedObjectTable 대신 Process FD 테이블 투영으로
// 전환) 전용 E2E 검증 클라이언트.
//
// 시나리오:
//  1. "/sys/live/named"를 Open(디렉터리)+Readdir로 베이스라인 개수
//     (N0)를 센다.
//  2. Socket(Stream)으로 소켓 하나를 만든다(Bind()는 하지 않는다 -
//     §4-1 자동 등록 경로 자체만 검증 대상, §4-2는 PN-E310E23A가
//     이미 socktest/sockclient로 검증했음).
//  3. 다시 Readdir - 정확히 N0+1개고, 새로 생긴 항목이
//     "<digits>/<digits>" 형식(kFormatAutoSocketPath 출력)인지 확인.
//  4. 그 이름으로 "/sys/live/named/<name>"을 Open해 성공(error==None)
//     하는지 확인 - livefs.cpp의 fd 테이블 즉석 조회(§4-1) 경로 자체를
//     검증하는 핵심 지점.
//  5. 원래 소켓 fd를 Close.
//  6. 다시 Readdir - N0개로 돌아왔는지 확인 - 별도 release() 호출
//     없이 fd 테이블 반납(fileDescriptors.erase)만으로 이 경로가
//     사라지는지가 이 계획의 핵심 주장이라 가장 중요한 검증 지점.
//
// exitCode: 0=전체 성공, 1=Socket 실패, 3=Readdir 스캔(디렉터리
// Open/Readdir 자체) 실패, 4=소켓 생성 후 새 항목을 못 찾음(개수
// 불일치 포함), 5=그 이름 Open 실패, 6=Close 후에도 개수가 안 줄어듦
// (좀비 - 이 계획이 고치려는 바로 그 버그가 재발했다는 뜻).
#include "libmc/socket.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kNamedDirPath[] = "/sys/live/named";

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

bool kIsDigit(char c) { return c >= '0' && c <= '9'; }

// kFormatAutoSocketPath(socket.cpp)가 만드는 "<pid>/<handle>" 형식인지만
// 확인한다 - 값 자체(정확한 pid/fd)는 검증하지 않는다(이 커널에
// Getpid류가 없어 자기 pid를 스스로 알 방법이 없음 - 형식 확인 +
// 개수 증감으로 충분히 검증 가능하다는 판단).
bool kLooksLikeAutoPath(const char* name, mc::uint32_t nameLen) {
    mc::uint32_t i = 0;
    if (i >= nameLen || !kIsDigit(name[i])) {
        return false;
    }
    while (i < nameLen && kIsDigit(name[i])) {
        ++i;
    }
    if (i >= nameLen || name[i] != '/') {
        return false;
    }
    ++i;
    if (i >= nameLen || !kIsDigit(name[i])) {
        return false;
    }
    while (i < nameLen && kIsDigit(name[i])) {
        ++i;
    }
    return i == nameLen;
}

struct ScanResult {
    mc::uint32_t count = 0;
    bool autoPathFound = false;
    char autoPathName[64] = {};
    mc::uint32_t autoPathNameLength = 0;
};

// 매번 새로 Open해서 처음부터 나열한다(Readdir의 커서는 그 fd 자신이
// 소유 - vfs_syscall.h ReaddirArgs 문서 주석 참고, 재사용하려면 다시
// 열어야 한다).
bool kScanNamedDir(ScanResult* out) {
    mc::OpenArgs openArgs;
    openArgs.path = kNamedDirPath;
    openArgs.pathLen = sizeof(kNamedDirPath) - 1;
    openArgs.flags = static_cast<mc::uint32_t>(mc::OpenFlags::Directory);
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointOpen, &openArgs);
    if (token == 0 || !mc::wait(token) || openArgs.error != mc::ChannelError::None) {
        return false;
    }

    *out = ScanResult{};
    bool ok = true;
    for (;;) {
        mc::ReaddirArgs readArgs;
        readArgs.fd = openArgs.fd;
        token = mc::submit(mc::kSyscallEndpointReaddir, &readArgs);
        if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None) {
            ok = false;
            break;
        }
        if (!readArgs.hasMore) {
            break;
        }
        ++out->count;
        if (!out->autoPathFound && kLooksLikeAutoPath(readArgs.name, readArgs.nameLength)) {
            out->autoPathFound = true;
            for (mc::uint32_t i = 0; i < readArgs.nameLength; ++i) {
                out->autoPathName[i] = readArgs.name[i];
            }
            out->autoPathNameLength = readArgs.nameLength;
        }
    }

    mc::CloseArgs closeArgs;
    closeArgs.fd = openArgs.fd;
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    if (token != 0) {
        mc::wait(token);
    }
    return ok;
}

}  // namespace

extern "C" void _start() {
    ScanResult baseline;
    if (!kScanNamedDir(&baseline)) {
        kFinish(3);
    }

    mc::SocketArgs socketArgs;
    socketArgs.domain = mc::SocketDomain::Unix;
    socketArgs.type = mc::SocketType::Stream;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSocket, &socketArgs);
    if (token == 0 || !mc::wait(token) || socketArgs.error != mc::ChannelError::None) {
        kFinish(1);
    }
    const mc::int32_t socketFd = static_cast<mc::int32_t>(socketArgs.fd);

    ScanResult afterCreate;
    if (!kScanNamedDir(&afterCreate)) {
        kFinish(3);
    }
    if (afterCreate.count != baseline.count + 1 || !afterCreate.autoPathFound) {
        kFinish(4);
    }

    char fullPath[128];
    mc::uint32_t pos = 0;
    for (mc::uint32_t i = 0; i < sizeof(kNamedDirPath) - 1; ++i) {
        fullPath[pos++] = kNamedDirPath[i];
    }
    fullPath[pos++] = '/';
    for (mc::uint32_t i = 0; i < afterCreate.autoPathNameLength; ++i) {
        fullPath[pos++] = afterCreate.autoPathName[i];
    }

    mc::OpenArgs openArgs;
    openArgs.path = fullPath;
    openArgs.pathLen = pos;
    openArgs.flags = 0;
    token = mc::submit(mc::kSyscallEndpointOpen, &openArgs);
    if (token == 0 || !mc::wait(token) || openArgs.error != mc::ChannelError::None) {
        kFinish(5);
    }
    // 이 Open이 연 것도 같은 Channel을 가리키는 별개 fd다 - 바로 정리.
    mc::CloseArgs openedCloseArgs;
    openedCloseArgs.fd = openArgs.fd;
    token = mc::submit(mc::kSyscallEndpointClose, &openedCloseArgs);
    if (token != 0) {
        mc::wait(token);
    }

    mc::CloseArgs socketCloseArgs;
    socketCloseArgs.fd = socketFd;
    token = mc::submit(mc::kSyscallEndpointClose, &socketCloseArgs);
    if (token == 0 || !mc::wait(token) || socketCloseArgs.error != mc::ChannelError::None) {
        kFinish(6);
    }

    ScanResult afterClose;
    if (!kScanNamedDir(&afterClose) || afterClose.count != baseline.count) {
        kFinish(6);
    }

    kFinish(0);
}
