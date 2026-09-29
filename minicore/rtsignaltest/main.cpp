// minicore/rtsignaltest - PN-FD706AF6(SP-A7479F83 §6-B, RT 신호 큐잉)
// E2E 검증 전용 클라이언트. signalfdtest와 동일한 방식(initrd에서
// rtsignalchild ELF를 읽어 실제 SpawnProcess) - 다른 점은 *같은* RT
// 신호 번호(SIGRT0=32)를 연속으로 3번 보내 표준 신호와 달리 전부
// 별도 인스턴스로 FIFO 순서대로 쌓이는지 확인한다는 것.
//
// RT 신호는 Process::signalMask/pendingSignals/체크포인트를 전혀
// 거치지 않으므로(signal.h kRtSignalBase 문서 주석) signalmasktest/
// signalfdtest와 달리 스폰 *전* 마스킹이 필요 없다 - 감시하는
// signalfd가 없으면 그냥 조용히 버려질 뿐, 일반 종료 경로로 새는
// 위험 자체가 없기 때문이다(그래도 자식이 SignalfdCreate를 마치기
// 전에 도착하면 그 인스턴스는 버려지므로, 스핀으로 타이밍을 벌어
// 주는 건 여전히 필요).
//
// exitCode: 1=SIGCHLD Ignore 실패, 2=initrd Stat/Open/Read/Close 실패,
// 3=rtsignalchild 엔트리 못 찾음, 4=SpawnProcess 실패, 5/6/7=1·2·3번째
// Kill 실패, 8/9/10=1·2·3번째 Kill error!=None, 11=Wait 타임아웃,
// 12=reapedPid 불일치, 13=자식 exitCode!=66(RT 큐잉이 실제로 동작
// 안 함 - 핵심 회귀), **99=전체 성공**.
#include "libcpio/cpio.h"
#include "libmc/process.h"
#include "libmc/signal.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr mc::int32_t kExpectedChildExitCode = 66;
constexpr mc::uint32_t kMaxWaitAttempts = 4000;
constexpr mc::uint32_t kSpinIterations = 2000;
constexpr mc::uint32_t kRtSignal0 = mc::kRtSignalBase;  // SIGRT0 = 32

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
    constexpr char kName[] = "rtsignalchild";
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

void kSendRt(mc::int64_t targetPid, mc::uint64_t userData, mc::int32_t failSubmitCode, mc::int32_t failErrorCode) {
    mc::KillArgs killArgs;
    killArgs.targetProcessId = targetPid;
    killArgs.signal = static_cast<mc::SignalNumber>(kRtSignal0);
    killArgs.userData = userData;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointKill, &killArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(failSubmitCode);
    }
    if (killArgs.error != mc::ChannelError::None) {
        kFinish(failErrorCode);
    }
}

}  // namespace

extern "C" void _start() {
    // 0) SIGCHLD Ignore.
    mc::SignalActionArgs sigArgs;
    sigArgs.signal = mc::SignalNumber::Chld;
    sigArgs.disposition = mc::SignalDisposition::Ignore;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSignalAction, &sigArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(1);
    }

    // 1) /sys/live/initrd.cpio에서 rtsignalchild ELF 읽기.
    constexpr char kInitrdPath[] = "/sys/live/initrd.cpio";
    constexpr mc::uint32_t kInitrdPathLen = sizeof(kInitrdPath) - 1;

    mc::StatArgs initrdStatArgs;
    initrdStatArgs.path = kInitrdPath;
    initrdStatArgs.pathLen = kInitrdPathLen;
    token = mc::submit(mc::kSyscallEndpointStat, &initrdStatArgs);
    if (token == 0 || !mc::wait(token) || initrdStatArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }
    if (initrdStatArgs.size > kMaxInitrdSize) {
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
    while (totalRead < initrdStatArgs.size) {
        mc::ReadArgs readArgs;
        readArgs.fd = openArgs.fd;
        readArgs.buf = gInitrdBuffer + totalRead;
        readArgs.len = static_cast<mc::uint32_t>(initrdStatArgs.size - totalRead);
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

    if (totalRead != initrdStatArgs.size) {
        kFinish(2);
    }

    FindChildContext findCtx;
    cpio::forEachEntry(gInitrdBuffer, totalRead, kFindChildEntry, &findCtx);
    if (!findCtx.found) {
        kFinish(3);
    }

    // 2) 자식 스폰(마스킹 불필요 - 위 문서 주석 참고).
    mc::SpawnProcessArgs spawnArgs;
    spawnArgs.imageBuffer = findCtx.data;
    spawnArgs.imageSize = findCtx.dataSize;
    token = mc::submit(mc::kSyscallEndpointSpawnProcess, &spawnArgs);
    if (token == 0 || !mc::wait(token) || spawnArgs.error != mc::SpawnProcessError::None) {
        kFinish(4);
    }
    const mc::int64_t childPid = spawnArgs.pid;

    // 3) 자식에게 SignalfdCreate+Read 진입 시간을 벌어 주는 스핀.
    for (mc::uint32_t i = 0; i < kSpinIterations; ++i) {
        mc::WaitArgs spinArgs;
        spinArgs.targetPid = -1;
        token = mc::submit(mc::kSyscallEndpointWait, &spinArgs);
        if (token != 0) {
            mc::wait(token);
        }
    }

    // 4) 같은 RT 신호를 연속 3회, 서로 다른 userData로.
    kSendRt(childPid, 100, 5, 8);
    kSendRt(childPid, 200, 6, 9);
    kSendRt(childPid, 300, 7, 10);

    // 5) 회수.
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
        kFinish(11);
    }
    if (waitArgs.reapedPid != childPid) {
        kFinish(12);
    }
    if (waitArgs.exitCode != kExpectedChildExitCode) {
        kFinish(13);
    }

    kFinish(99);
}
