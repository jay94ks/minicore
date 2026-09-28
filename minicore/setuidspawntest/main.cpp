// minicore/setuidspawntest - PN-2A0981B7 항목4(EXEC_SETUID 승격 경로
// E2E 검증) 전용 클라이언트. exitwaiter(PN-5EDE3C96)와 동일한 방식
// (initrd에서 exittest ELF를 읽어 실제 SpawnProcess) - 다른 점은
// `imagePath`가 `/sys/live/initrd.cpio`가 아니라 실제 AHCI 디스크
// (ext4, diskmetatest와 같은 이미지)의 `setuidmarker` 파일을 가리킨다
// 는 것 - 이 파일은 S 비트(mode bit9)+uid=66으로 debugfs가 미리
// 심어 둔 것으로, 실제 파일 내용(마커 텍스트)은 실행되지 않는다
// (imageBuffer는 여전히 exittest ELF - process.cpp의 SpawnProcessHandler
// 문서 주석 참고, imagePath는 오직 S 비트/소유자 판정에만 쓰인다).
//
// **정직한 검증 범위**: 이 프로그램(유저랜드)은 SpawnProcess가
// imagePath 인자를 받아 실패 없이 자식을 실제로 실행하고, 부모가
// 그 종료를 정상적으로 회수하는 파이프라인 전체가 깨지지 않았음만
// 검증한다(exitCode==77). **승격된 uid가 정확히 66인지는 이 프로그램
// 자체로는 확인할 수 없다** - 이 커널에 아직 Getuid류 syscall이 없어
// 자식이 스스로 uid를 보고할 방법이 없기 때문이다(process.h 참고,
// RM-48E1E610에 미예약). 이번 세션은 process.cpp의 EXEC_SETUID
// 승격 지점에 TEMP Logger::info를 심어 실제 승격값(66)을 커널
// 로그로 직접 확인했다(검증 후 원복) - 후속 세션이 Getuid를 추가하면
// 이 테스트를 자식 스스로 uid를 보고하도록(exitCode에 실어) 확장할
// 수 있다.
//
// exitCode: 0=전체 성공(파이프라인 정상), 1=SIGCHLD Ignore 실패,
// 2=initrd Stat/Open/Read/Close 실패, 3=exittest 엔트리 못 찾음,
// 4=setuidmarker Stat 실패(부팅 경합 예산 소진), 5=SpawnProcess 실패,
// 6=Wait 타임아웃, 7=reapedPid 불일치, 8=exitCode!=77.
#include "libcpio/cpio.h"
#include "libmc/process.h"
#include "libmc/signal.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr mc::int32_t kExpectedChildExitCode = 77;
constexpr mc::uint32_t kMaxWaitAttempts = 2000;
constexpr mc::uint32_t kMaxRetriesForBootRace = 200;

constexpr char kSetuidMarkerPath[] = "/sys/mnt/setuidmarker";
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
    // 0) SIGCHLD Ignore - exitwaiter와 동일한 이유.
    mc::SignalActionArgs sigArgs;
    sigArgs.signal = mc::SignalNumber::Chld;
    sigArgs.disposition = mc::SignalDisposition::Ignore;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSignalAction, &sigArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(1);
    }

    // 1) /sys/live/initrd.cpio에서 exittest ELF 읽기(exitwaiter와 동일 관례).
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

    // 2) setuidmarker(S 비트+uid=66, debugfs로 심어 둔 실제 ext4 파일)가
    //    fs KernelThread의 비동기 AHCI 마운트 완료 후에야 보이므로
    //    diskmetatest와 동일한 부팅 경합 재시도.
    mc::StatArgs markerStatArgs;
    for (mc::uint32_t attempt = 0; attempt < kMaxRetriesForBootRace; ++attempt) {
        markerStatArgs = mc::StatArgs{};
        markerStatArgs.path = kSetuidMarkerPath;
        markerStatArgs.pathLen = sizeof(kSetuidMarkerPath) - 1;
        token = mc::submit(mc::kSyscallEndpointStat, &markerStatArgs);
        if (token == 0 || !mc::wait(token)) {
            kFinish(4);
        }
        if (markerStatArgs.error == mc::ChannelError::None) {
            break;
        }
    }
    if (markerStatArgs.error != mc::ChannelError::None) {
        kFinish(4);
    }

    // 3) SpawnProcess - imagePath가 setuidmarker를 가리켜야 커널이
    //    EXEC_SETUID 승격 판정을 그 파일 기준으로 한다(process.cpp).
    mc::SpawnProcessArgs spawnArgs;
    spawnArgs.imageBuffer = findCtx.data;
    spawnArgs.imageSize = findCtx.dataSize;
    spawnArgs.flags = mc::SpawnProcessFlags::kSpawnAllowSetuid;
    spawnArgs.imagePath = kSetuidMarkerPath;
    spawnArgs.imagePathLen = sizeof(kSetuidMarkerPath) - 1;
    token = mc::submit(mc::kSyscallEndpointSpawnProcess, &spawnArgs);
    if (token == 0 || !mc::wait(token) || spawnArgs.error != mc::SpawnProcessError::None) {
        kFinish(5);
    }
    const mc::int64_t childPid = spawnArgs.pid;

    // 4) Wait() 폴링 - exitwaiter와 동일.
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
        kFinish(6);
    }
    if (waitArgs.reapedPid != childPid) {
        kFinish(7);
    }
    if (waitArgs.exitCode != kExpectedChildExitCode) {
        kFinish(8);
    }

    kFinish(0);
}
