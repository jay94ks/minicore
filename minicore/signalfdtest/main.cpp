// minicore/signalfdtest - PN-A1A0B595(SP-A7479F83 §2/§3/§4, signalfd)
// E2E 검증 전용 클라이언트. signalmasktest와 동일한 방식(initrd에서
// signalfdchild ELF를 읽어 실제 SpawnProcess) - 다른 점은 자식이
// signalfd로 신호를 "읽어서" 확인한다는 것(signalmasktest는 자식이
// 일반 종료 경로를 피해 스스로 종료하는지만 확인).
//
// 1) 부모가 스폰 *전* 자기 마스크에 SIGTERM을 Block(SP-0666DB3C §4.6
//    상속 - 자식이 태어나는 순간부터 이미 블록돼 있어 부모의 Kill이
//    도착하기 전에 자식이 SignalfdCreate를 마칠 시간을 벌어 준다).
// 2) 자식(signalfdchild)을 스폰 - 자식은 SignalfdCreate(SIGTERM)로
//    signalfd를 열고 Read로 블로킹.
// 3) 부모가 스핀(kSpinIterations회 - 자식에게 스케줄링 기회를 줌) 후
//    Kill(child, Terminate).
// 4) 자식이 그 신호를 signalfd Read로 정확히 받아 exitCode=55로 종료
//    했는지 Wait로 확인.
//
// exitCode: 1=SIGCHLD Ignore 실패, 2=initrd Stat/Open/Read/Close 실패,
// 3=signalfdchild 엔트리 못 찾음, 4=SignalMask(Block) 실패,
// 5=SpawnProcess 실패, 6/7=Kill 실패/error!=None, 8=Wait 타임아웃,
// 9=reapedPid 불일치, 10=자식 exitCode!=55(signalfd가 신호를 제대로
// 못 받음 - 핵심 회귀), **99=전체 성공**.
#include "libcpio/cpio.h"
#include "libmc/process.h"
#include "libmc/signal.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr mc::int32_t kExpectedChildExitCode = 55;
constexpr mc::uint32_t kMaxWaitAttempts = 4000;
constexpr mc::uint32_t kSpinIterations = 2000;

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
    constexpr char kName[] = "signalfdchild";
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
    // 0) SIGCHLD Ignore.
    mc::SignalActionArgs sigArgs;
    sigArgs.signal = mc::SignalNumber::Chld;
    sigArgs.disposition = mc::SignalDisposition::Ignore;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSignalAction, &sigArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(1);
    }

    // 1) /sys/live/initrd.cpio에서 signalfdchild ELF 읽기.
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

    // 2) 스폰 전 자기 마스크에 SIGTERM Block - 자식이 상속받는다.
    mc::SignalMaskArgs maskArgs;
    maskArgs.op = mc::SignalMaskOp::Block;
    maskArgs.mask = 1u << static_cast<mc::uint32_t>(mc::SignalNumber::Terminate);
    token = mc::submit(mc::kSyscallEndpointSignalMask, &maskArgs);
    if (token == 0 || !mc::wait(token) || maskArgs.error != mc::ChannelError::None) {
        kFinish(4);
    }

    // 3) 자식 스폰.
    mc::SpawnProcessArgs spawnArgs;
    spawnArgs.imageBuffer = findCtx.data;
    spawnArgs.imageSize = findCtx.dataSize;
    token = mc::submit(mc::kSyscallEndpointSpawnProcess, &spawnArgs);
    if (token == 0 || !mc::wait(token) || spawnArgs.error != mc::SpawnProcessError::None) {
        kFinish(5);
    }
    const mc::int64_t childPid = spawnArgs.pid;

    // 4) 자식에게 SignalfdCreate+Read 진입 시간을 벌어 주는 스핀
    //    (kMaxRetriesForBootRace류 다른 테스트와 동일한 관례).
    for (mc::uint32_t i = 0; i < kSpinIterations; ++i) {
        mc::WaitArgs spinArgs;
        spinArgs.targetPid = -1;
        token = mc::submit(mc::kSyscallEndpointWait, &spinArgs);
        if (token != 0) {
            mc::wait(token);
        }
    }

    // 5) Kill(child, Terminate).
    mc::KillArgs killArgs;
    killArgs.targetProcessId = childPid;
    killArgs.signal = mc::SignalNumber::Terminate;
    token = mc::submit(mc::kSyscallEndpointKill, &killArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(6);
    }
    if (killArgs.error != mc::ChannelError::None) {
        kFinish(7);
    }

    // 6) 회수.
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
        kFinish(8);
    }
    if (waitArgs.reapedPid != childPid) {
        kFinish(9);
    }
    if (waitArgs.exitCode != kExpectedChildExitCode) {
        kFinish(10);
    }

    kFinish(99);
}
