// minicore/exitwaiter - PN-5EDE3C96 항목1(Process::exitCode가 항상
// 0으로 고정되던 문제의 배선 완료) 실측 검증 하네스 - dbgdriver/
// sockinherit과 동일한 지위("정상적인 커널 서비스가 아니다, 부팅
// 매니페스트에 절대 등록하지 않는다", TEMP kmain.cpp 훅으로만 자동
// 실행).
//
// 시나리오: /sys/live/initrd.cpio에서 "exittest" ELF를 읽어(dbgdriver/
// sockinherit과 동일한 관례) 실제 SpawnProcess로 스폰한다 - 이 스폰
// 호출의 caller가 바로 이 프로세스이므로 커널이 exittest->parent를
// 이 프로세스로 정확히 채운다(SpawnProcessHandler 관례) - 그래야
// kFinalizeProcessTermination의 "부모가 있으면 좀비로 남겨 exitCode를
// 물려준다" 분기(SP-76250478 §3.2)를 실제로 탄다. exittest는
// mc::selfTerminate(77)로 즉시 종료하므로, 이 프로세스가 Wait()로
// 그 좀비를 회수했을 때 reapedPid가 스폰한 pid와 일치하고
// exitCode==77이면 배선이 실제로 동작한다는 뜻이다.
//
// SIGCHLD 기본 disposition은 종료(init.cpp/sockinherit이 이미 문서화한
// 전제) - 이 프로세스도 자식을 낳으므로 반드시 Ignore로 바꿔야 한다.
//
// exitCode 규약(TEMP kernel-side 훅이 이 프로세스 자신의 UserThread::
// exitCode를 직접 읽어 Logger::info로 보고한다 - process->threads가
// 아직 release되지 않은 채 남아있는 "부모 없는 TEMP 스폰" 관례를
// 그대로 이용, 별도 console/procfs 불필요):
//   0 = 전 구간 정상(SpawnProcess/Wait 성공 + reapedPid 일치 + exitCode==77)
//   1 = SIGCHLD Ignore 설정 실패
//   2 = /sys/live/initrd.cpio Stat/Open/Read/Close 실패
//   3 = cpio 아카이브 안에서 "exittest" 엔트리를 못 찾음
//   4 = SpawnProcess 실패
//   5 = Wait 폴링이 kMaxWaitAttempts 안에 좀비를 못 찾음(타임아웃)
//   6 = reapedPid가 스폰한 pid와 다름(다른 자식을 잘못 회수 - 이론상
//       도달 불가, 이 프로세스의 유일한 자식이 exittest뿐이므로)
//   7 = exitCode != 77(핵심 회귀 - 배선이 여전히 깨져 있다는 뜻)
#include "libcpio/cpio.h"
#include "libmc/process.h"
#include "libmc/signal.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr mc::int32_t kExpectedExitCode = 77;
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
    constexpr char kName[] = "exittest";
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
    // 0) SIGCHLD Ignore - sockinherit/init과 동일한 이유.
    mc::SignalActionArgs sigArgs;
    sigArgs.signal = mc::SignalNumber::Chld;
    sigArgs.disposition = mc::SignalDisposition::Ignore;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSignalAction, &sigArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(1);
    }

    // 1) /sys/live/initrd.cpio에서 exittest ELF 읽기(sockinherit과 동일 관례).
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

    // 2) SpawnProcess - 이 프로세스가 caller이므로 exittest->parent는
    //    정확히 이 프로세스가 된다.
    mc::SpawnProcessArgs spawnArgs;
    spawnArgs.imageBuffer = findCtx.data;
    spawnArgs.imageSize = findCtx.dataSize;
    token = mc::submit(mc::kSyscallEndpointSpawnProcess, &spawnArgs);
    if (token == 0 || !mc::wait(token) || spawnArgs.error != mc::SpawnProcessError::None) {
        kFinish(4);
    }
    const mc::int64_t childPid = spawnArgs.pid;

    // 3) Wait() 폴링 - init.cpp와 동일한 이유로 논블로킹 ABI라 짧게
    //    반복한다(exittest는 즉시 selfTerminate하므로 대개 몇 번 안에
    //    좀비가 보여야 한다).
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
    if (waitArgs.exitCode != kExpectedExitCode) {
        kFinish(7);
    }

    kFinish(0);
}
