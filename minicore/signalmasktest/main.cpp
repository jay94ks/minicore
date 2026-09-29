// minicore/signalmasktest - PN-A1A0B595(signalfd) 선행 작업,
// QU-28D7C7B1 답변(A)(SP-0666DB3C §4.6, 프로세스 시그널 마스크) E2E
// 검증 전용 클라이언트. setuidspawntest/exitwaiter와 동일한 방식
// (initrd에서 signalmasktarget ELF를 읽어 실제 SpawnProcess).
//
// 두 시나리오를 순서대로 검증한다 - 둘 다 signalmasktarget(자기
// 마스크를 절대 안 건드리는 자식, kSpinIterations회 스핀 후
// selfTerminate(42))을 스폰하고 즉시 Kill(child, Terminate)을 보낸다:
//
//  A) **마스크 시나리오**: 부모가 스폰 *전에* SignalMask(Block,
//     1<<Terminate)로 자기 마스크를 걸어 둔다 - SpawnProcess가
//     signalMask를 그대로 자식에게 물려주므로(SP-0666DB3C §4.6 상속),
//     자식은 태어나는 순간부터 이미 Terminate가 블록된 상태다. Kill이
//     보내는 신호는 자식의 pendingSignals에 쌓이기만 하고 체크포인트가
//     건너뛰어 스핀 루프가 방해받지 않는다 - exitCode=42(자식이 스스로
//     제어된 종료에 도달) 기대.
//  B) **대조군(마스크 없음)**: 부모가 자기 마스크를 SetMask(0)으로
//     되돌린 뒤 같은 과정을 반복 - 이번엔 자식이 마스크를 안 물려받아
//     다음 체크포인트에서 커널이 강제 종료(exitCode=0 고정,
//     RM-48E1E610 Process 그룹 0번 참고)시킨다 - 이 테스트가 마스킹의
//     부재를 실제로 검출할 수 있음을 증명하는 negative-control.
//
// exitCode: **99=전체 성공**(0이 아니다 - 0은 신호/폴트 강제종료의
// 고정 exitCode, RM-48E1E610 Process 0번과 겹쳐서 "진짜 성공"과
// "이 프로세스 자신이 도중에 강제종료됨"을 구분 못 하게 되므로
// 의도적으로 피함). 1=SIGCHLD Ignore 실패, 2=initrd Stat/Open/Read/
// Close 실패, 3=signalmasktarget 엔트리 못 찾음, 10=A) SignalMask
// (Block) 실패, 11=A) SpawnProcess 실패, 12/13=A) Kill 실패/error!=None,
// 14=A) Wait 타임아웃, 15=A) reapedPid 불일치, 16=A) exitCode!=42
// (마스킹이 실제로 동작 안 함 - 핵심 회귀), 20=B) SignalMask(SetMask)
// 실패, 21=B) SpawnProcess 실패, 22/23=B) Kill 실패/error!=None,
// 24=B) Wait 타임아웃, 25=B) reapedPid 불일치, 26=B) exitCode!=0
// (마스크가 리셋 안 되고 새어 나감 - negative-control 실패).
#include "libcpio/cpio.h"
#include "libmc/process.h"
#include "libmc/signal.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr mc::int32_t kExpectedMaskedExitCode = 42;
constexpr mc::uint32_t kMaxWaitAttempts = 4000;

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
    constexpr char kName[] = "signalmasktarget";
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

// 자식을 스폰하고 즉시 Kill(Terminate)을 보낸 뒤 회수한다 - 성공하면
// out에 exitCode를 채우고 true, 실패하면 kFinish(baseErrorCode+n)로
// 즉시 종료(n은 실패 지점별 0/1/2/3/4).
void kSpawnKillAndWait(const void* imageData, mc::uint64_t imageSize, mc::int32_t baseErrorCode,
                       mc::int32_t* outExitCode) {
    mc::SpawnProcessArgs spawnArgs;
    spawnArgs.imageBuffer = imageData;
    spawnArgs.imageSize = imageSize;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSpawnProcess, &spawnArgs);
    if (token == 0 || !mc::wait(token) || spawnArgs.error != mc::SpawnProcessError::None) {
        kFinish(baseErrorCode + 1);
    }
    const mc::int64_t childPid = spawnArgs.pid;

    mc::KillArgs killArgs;
    killArgs.targetProcessId = childPid;
    killArgs.signal = mc::SignalNumber::Terminate;
    token = mc::submit(mc::kSyscallEndpointKill, &killArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(baseErrorCode + 2);
    }
    if (killArgs.error != mc::ChannelError::None) {
        kFinish(baseErrorCode + 3);
    }

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
        kFinish(baseErrorCode + 4);
    }
    if (waitArgs.reapedPid != childPid) {
        kFinish(baseErrorCode + 5);
    }
    *outExitCode = waitArgs.exitCode;
}

}  // namespace

extern "C" void _start() {
    // 0) SIGCHLD Ignore - 부모 자신이 자식의 종료 신호로 죽지 않도록
    //    (exitwaiter/setuidspawntest와 동일한 이유).
    mc::SignalActionArgs sigArgs;
    sigArgs.signal = mc::SignalNumber::Chld;
    sigArgs.disposition = mc::SignalDisposition::Ignore;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSignalAction, &sigArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(1);
    }

    // 1) /sys/live/initrd.cpio에서 signalmasktarget ELF 읽기(exitwaiter/
    //    setuidspawntest와 동일 관례).
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

    // A) 마스크 시나리오 - 스폰 전에 자기 마스크를 걸어 둔다.
    mc::SignalMaskArgs maskArgs;
    maskArgs.op = mc::SignalMaskOp::Block;
    maskArgs.mask = 1u << static_cast<mc::uint32_t>(mc::SignalNumber::Terminate);
    token = mc::submit(mc::kSyscallEndpointSignalMask, &maskArgs);
    if (token == 0 || !mc::wait(token) || maskArgs.error != mc::ChannelError::None) {
        kFinish(10);
    }

    mc::int32_t maskedExitCode = -1;
    kSpawnKillAndWait(findCtx.data, findCtx.dataSize, 10, &maskedExitCode);
    if (maskedExitCode != kExpectedMaskedExitCode) {
        kFinish(16);
    }

    // B) 대조군 - 자기 마스크를 되돌린 뒤 반복(negative-control).
    mc::SignalMaskArgs resetArgs;
    resetArgs.op = mc::SignalMaskOp::SetMask;
    resetArgs.mask = 0;
    token = mc::submit(mc::kSyscallEndpointSignalMask, &resetArgs);
    if (token == 0 || !mc::wait(token) || resetArgs.error != mc::ChannelError::None) {
        kFinish(20);
    }

    mc::int32_t unmaskedExitCode = -1;
    kSpawnKillAndWait(findCtx.data, findCtx.dataSize, 20, &unmaskedExitCode);
    if (unmaskedExitCode != 0) {
        kFinish(26);
    }

    // [주의] 99를 쓴다 - 0은 신호/폴트 강제종료의 고정 exitCode
    // (RM-48E1E610 Process 0번)와 겹쳐 "정말 끝까지 성공했는지"와
    // "이 프로세스 자신이 도중에 강제종료됐는지"를 구분할 수 없다.
    kFinish(99);
}
