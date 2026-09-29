// minicore/socknamedwaiter - PN-F9CBF1A9 검증 하네스, exitwaiter와
// 동일한 패턴("정상적인 커널 서비스가 아니다, 부팅 매니페스트에 절대
// 등록하지 않는다", TEMP kmain.cpp 훅으로만 자동 실행).
//
// **왜 필요한가(실측으로 발견)**: 이 계획(§4-1)의 fd 테이블 투영은
// `Process::resolveById()`/`Process::forEachLive()`로 살아있는
// 프로세스를 찾는데, 이 둘 다 `gProcessTable`(실제 `SpawnProcess`
// 경로에서만 `kAllocateProcessId()`로 채워짐)에 의존한다. TEMP
// kmain.cpp 훅이 socknamedtest를 곧바로 "고정 스폰 KernelService"로
// 띄우면(socktest/sockclient와 동일한 방식) 그 프로세스는
// `processId==kInvalidProcessId`로 남아(`process.cpp`의 `Process::
// init()` 문서 주석 - "고정 스폰 KernelService는 SpawnProcess 경로를
// 안 타 kAllocateProcessId()를 절대 안 부른다") 자기 자신의 소켓조차
// 못 찾는다(exitCode=4로 실측 확인) - 이건 §4-1 설계의 결함이 아니라
// "실제 SpawnProcess로 낳은 평범한 프로세스"만 이 투영의 대상이 될
// 수 있다는 뜻일 뿐이다(커널 서비스 자신이 만드는 소켓의 발견 경로는
// SP-231493CB §4-1이 이미 "착수 세션이 각 서비스에 맞게 구현"으로
// 명시적으로 열어 둔 별도 범위 - PN-F9CBF1A9 항목4 참고). 그래서
// exitwaiter와 똑같이 이 waiter가 /sys/live/initrd.cpio에서
// "socknamedtest" ELF를 읽어 진짜 SpawnProcess로 자식을 낳고(그래야
// gProcessTable에 진짜 ProcessId가 발급됨), 그 결과만 대신 보고한다.
//
// exitCode: 이 waiter 자신의 문제는 1(SIGCHLD Ignore 실패)/
// 2(initrd Stat/Open/Read/Close 실패)/3(cpio에서 "socknamedtest"
// 못 찾음)/4(SpawnProcess 실패)/5(Wait 타임아웃)/6(reapedPid 불일치) -
// 그 외에는 자식(socknamedtest)의 실제 exitCode를 그대로 전달한다
// (0=전체 성공, 1/3/4/5/6=socknamedtest/main.cpp 상단 주석의 그
// 번호와 동일한 의미).
#include "libcpio/cpio.h"
#include "libmc/process.h"
#include "libmc/signal.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr mc::uint32_t kMaxWaitAttempts = 2000;  // 다른 TEMP 하네스와 동일한 부팅 경합 여유
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
    constexpr char kName[] = "socknamedtest";
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
    mc::SignalActionArgs sigArgs;
    sigArgs.signal = mc::SignalNumber::Chld;
    sigArgs.disposition = mc::SignalDisposition::Ignore;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSignalAction, &sigArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(1);
    }

    constexpr char kInitrdPath[] = "/sys/live/initrd.cpio";
    constexpr mc::uint32_t kInitrdPathLen = sizeof(kInitrdPath) - 1;

    mc::StatArgs statArgs;
    statArgs.path = kInitrdPath;
    statArgs.pathLen = kInitrdPathLen;
    token = mc::submit(mc::kSyscallEndpointStat, &statArgs);
    if (token == 0 || !mc::wait(token) || statArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }
    if (statArgs.size > kMaxInitrdSize) {
        kFinish(2);
    }

    mc::OpenArgs openArgs;
    openArgs.path = kInitrdPath;
    openArgs.pathLen = kInitrdPathLen;
    openArgs.flags = static_cast<mc::uint32_t>(mc::OpenFlags::ReadOnly);
    token = mc::submit(mc::kSyscallEndpointOpen, &openArgs);
    if (token == 0 || !mc::wait(token) || openArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }

    mc::uint64_t totalRead = 0;
    while (totalRead < statArgs.size) {
        mc::ReadArgs readArgs;
        readArgs.fd = openArgs.fd;
        readArgs.buf = gInitrdBuffer + totalRead;
        readArgs.len = static_cast<mc::uint32_t>(statArgs.size - totalRead);
        token = mc::submit(mc::kSyscallEndpointRead, &readArgs);
        if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None) {
            kFinish(2);
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
        kFinish(2);
    }

    FindChildContext findCtx;
    cpio::forEachEntry(gInitrdBuffer, totalRead, kFindChildEntry, &findCtx);
    if (!findCtx.found) {
        kFinish(3);
    }

    mc::SpawnProcessArgs spawnArgs;
    spawnArgs.imageBuffer = findCtx.data;
    spawnArgs.imageSize = findCtx.dataSize;
    token = mc::submit(mc::kSyscallEndpointSpawnProcess, &spawnArgs);
    if (token == 0 || !mc::wait(token) || spawnArgs.error != mc::SpawnProcessError::None) {
        kFinish(4);
    }
    const mc::int64_t childPid = spawnArgs.pid;

    mc::WaitArgs waitArgs;
    bool reaped = false;
    for (mc::uint32_t attempt = 0; attempt < kMaxWaitAttempts; ++attempt) {
        waitArgs = mc::WaitArgs{};
        waitArgs.targetPid = childPid;
        token = mc::submit(mc::kSyscallEndpointWait, &waitArgs);
        if (token != 0 && mc::wait(token) && waitArgs.hadZombieChild) {
            reaped = true;
            break;
        }
        asm volatile("pause");
    }
    if (!reaped) {
        kFinish(5);
    }
    if (waitArgs.reapedPid != childPid) {
        kFinish(6);
    }

    // 자식(socknamedtest)의 실제 exitCode를 그대로 전달한다.
    kFinish(static_cast<mc::int32_t>(waitArgs.exitCode));
}
