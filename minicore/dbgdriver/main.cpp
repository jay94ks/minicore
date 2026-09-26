// minicore/dbgdriver - PN-0556C759(멀티스레드 하드웨어 브레이크포인트
// + 반복 DebugContinue 경쟁 재현) 전용 디버거 드라이버 프로그램.
// minicore/dbgtarget을 실제 SpawnProcess syscall로 자식으로 스폰해(이
// 프로세스 자신이 그 진짜 직계 부모가 되므로 DebugAttach의
// kFindDebuggableChild 권한 검증을 통과한다 - dbgtarget 상단 주석
// 참고) 브레이크포인트 hit -> 스레드0/스레드1 레지스터 조회 ->
// DebugContinue(all-stop -> continue-all) 왕복을 반복하며 Double
// Fault/손상된 Page Fault 재현을 시도한다.
//
// **정상적인 커널 서비스가 아니다** - dbgtarget/proctest와 마찬가지로
// 부팅 매니페스트(kmain.cpp의 gServiceManifest)에는 없고, initrd
// 안에는 원본 ELF 바이트로만 실린다(scripts/build-initrd.sh). 이
// 프로세스 자신은 DebugAttach의 대상이 아니므로(dbgtarget과 달리)
// parent=nullptr(커널이 직접 execImage로 스폰)이어도 무방하다 -
// dbgtarget이 요구하는 "실제 SpawnProcess 호출자"는 바로 이
// 프로세스 자신이 되어 주는 쪽이라, dbgtarget처럼 반드시 SpawnProcess
// 경유로 스폰될 필요는 없다.
//
// **실행하려면 TEMP 보팅 훅이 필요하다** - 2026-09-26 세션이 실제로
// 검증한 방법: kmain.cpp에 "init"과 동일한 특수 케이스로 이름
// "dbgdriver"를 매치하는 전용 정적 버퍼(gServiceManifest와는 별개 -
// KernelService essential=true를 쓰면 이 프로그램이 스트레스 루프를
// 마치고 정상 종료할 때마다 커널이 패닉한다)를 두고, kSpawnInitProcess()
// 를 본뜬 스폰 함수(role/essential은 Process::allocate()의 memset(0)
// 결과 그대로 - Normal/false)를 kSpawnServiceProcesses() 직후에서
// 호출한다. 조사가 끝나면 그 훅은 되돌린다(이 파일 자체와 CMake/
// build-initrd.sh 배선은 dbgtarget/proctest와 동일하게 영구 유지) -
// 자세한 배선은 PN-0556C759 본문의 "재현 방법" 절 참고.
#include "libcpio/cpio.h"
#include "libmc/debug.h"
#include "libmc/process.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

// exitCode 규약(주의: 2026-09-26 실측으로 확인 - v1은 `mc::selfTerminate()`
// 의 exitCode 자체를 커널이 아직 안 쓴다, syscall.cpp 문서 주석 참고 -
// 이 프로세스는 부모도 없어 Wait()로 관측될 일도 없다. 이 규약은
// 참고용 문서일 뿐, 실제 진행 상황 확인은 위 TEMP 보팅 훅과 함께
// debug_session.cpp/process.cpp 성공 경로에 TEMP Logger::info를
// 임시로 심어서 한다 - 2026-09-26 세션이 실제로 이렇게 확인했다):
//   0 = 60라운드 전부 정상 완주(재현 안 됨 - 이번 시도에서는 경쟁이
//       나타나지 않았다는 뜻이지 버그가 없다는 증명은 아니다)
//   1 = /sys/live/initrd.cpio Stat/Open/Read/Close 중 하나 실패
//       (submit/wait 실패 또는 error!=None 또는 길이 불일치)
//   2 = cpio 아카이브 안에서 "dbgtarget" 엔트리를 못 찾음
//   3 = SpawnProcess 실패(submit/wait 실패 또는 error!=None)
//   4 = DebugAttach 실패
//   5 = DebugSetBreakpoint 실패
//   6 = 초기 DebugContinue(정지 상태로 스폰된 스레드0 최초 재개) 실패
//   7 = 스트레스 루프 도중 스레드0/스레드1이 브레이크포인트에 도달하길
//       기다리다 타임아웃(kMaxPollAttempts) 또는 GetRegisters가
//       NotFound 외의 에러로 실패
//   8 = 스트레스 루프 도중 DebugContinue 실패
//       (커널이 패닉/더블폴트로 죽었다면 애초에 이 반환 자체가 없다 -
//       그 경우는 QEMU 콘솔 로그로 직접 확인해야 한다)
namespace {

// nm/objdump -t로 실제 빌드된 dbgtarget에서 kBreakpointTarget의 가상
// 주소를 뽑아 채운다(이 커널엔 ELF symtab 파서가 없어 ET_EXEC 고정
// 링크 특성을 이용한 수작업 방식 - PN-0556C759 "재현 방법" 참고).
// [TEMP, 2026-09-26 실측] `nm build-userland/minicore-dbgtarget/dbgtarget`
// 결과: `0000000000400320 t _ZN12_GLOBAL__N_117kBreakpointTargetEm` -
// dbgtarget/main.cpp이 바뀌어 재빌드되면 이 주소도 달라질 수 있으니
// dbgtarget을 다시 빌드했다면 이 상수도 다시 확인한다.
constexpr mc::uint64_t kBreakpointTargetAddress = 0x400320;

constexpr mc::uint32_t kStressRounds = 60;

// kSpawnDebugStart로 정지된 채 시작한 스레드0이 실제로 브레이크포인트에
// 도달(=#DB로 정지, debugLiveFramePtr 확보)하기까지, 그리고 스레드1이
// CreateThread로 실제 생성되고 마찬가지로 브레이크포인트에 도달하기
// 까지는 이 debug syscall API에 별도 "정지 알림" 채널이 없어(debug.h
// 참고 - 전부 요청/응답 syscall뿐) 폴링으로 기다려야 한다 - GetRegisters
// 가 NotFound를 돌려주는 동안 재시도한다.
constexpr mc::uint32_t kMaxPollAttempts = 2000000;

// 폴링하며 DebugGetRegisters를 반복 - NotFound(아직 안 멈췄거나 스레드가
// 아직 없음)면 재시도, 그 외 에러거나 진짜 submit/wait 실패면 즉시
// 포기(재시도해도 의미 없음). 성공하면 true.
bool kPollGetRegisters(mc::int64_t pid, mc::ThreadId targetThread, mc::DebugRegisterSnapshot& out) {
    for (mc::uint32_t attempt = 0; attempt < kMaxPollAttempts; ++attempt) {
        mc::DebugGetRegistersArgs args;
        args.targetProcessId = pid;
        args.targetThread = targetThread;
        args.out = &out;
        mc::SyscallToken token = mc::submit(mc::kSyscallEndpointDebugGetRegisters, &args);
        if (token == 0 || !mc::wait(token)) {
            return false;
        }
        if (args.error == mc::ChannelError::None) {
            return true;
        }
        if (args.error != mc::ChannelError::NotFound) {
            return false;
        }
    }
    return false;
}

// DebugContinue도 같은 이유로 폴링한다 - pausedByDebugger가 아직 0(예:
// 두 스레드 중 하나만 멈추고 다른 하나는 아직 도달 전)이면 NotFound가
// 돌아올 수 있다(debug_session.cpp DebugContinueHandler 참고).
bool kPollDebugContinue(mc::int64_t pid) {
    for (mc::uint32_t attempt = 0; attempt < kMaxPollAttempts; ++attempt) {
        mc::DebugContinueArgs args;
        args.targetProcessId = pid;
        mc::SyscallToken token = mc::submit(mc::kSyscallEndpointDebugContinue, &args);
        if (token == 0 || !mc::wait(token)) {
            return false;
        }
        if (args.error == mc::ChannelError::None) {
            return true;
        }
        if (args.error != mc::ChannelError::NotFound) {
            return false;
        }
    }
    return false;
}

// 유저랜드에 동적 힙 할당이 아직 없어(SP-8B6B8D25 §5 v1 범위) 다른
// 유저 프로그램들과 동일하게 고정 크기 static 버퍼 관례를 따른다
// (kmain.cpp의 gInitImageBuffer류와 같은 이유) - 이 커널의 initrd는
// 지금까지 전부 이 크기 아래였다(build-initrd.sh 참고).
constexpr mc::uint64_t kMaxInitrdSize = 4 * 1024 * 1024;
mc::uint8_t gInitrdBuffer[kMaxInitrdSize];

struct FindDbgTargetContext {
    const void* data = nullptr;
    mc::uint64_t dataSize = 0;
    bool found = false;
};

void kFindDbgTargetEntry(const cpio::Entry& entry, void* userData) {
    auto* ctx = static_cast<FindDbgTargetContext*>(userData);
    if (ctx->found) {
        return;
    }
    constexpr char kName[] = "dbgtarget";
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
    // 1) /sys/live/initrd.cpio 읽기 - Stat으로 크기 확인 후
    //    Open+반복 Read+Close(proctest/main.cpp와 동일한 관례).
    constexpr char kInitrdPath[] = "/sys/live/initrd.cpio";
    constexpr mc::uint32_t kInitrdPathLen = sizeof(kInitrdPath) - 1;

    mc::StatArgs statArgs;
    statArgs.path = kInitrdPath;
    statArgs.pathLen = kInitrdPathLen;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointStat, &statArgs);
    if (token == 0 || !mc::wait(token) || statArgs.error != mc::ChannelError::None) {
        kFinish(1);
    }
    if (statArgs.size > kMaxInitrdSize) {
        kFinish(1);
    }

    mc::OpenArgs openArgs;
    openArgs.path = kInitrdPath;
    openArgs.pathLen = kInitrdPathLen;
    openArgs.flags = static_cast<mc::uint32_t>(mc::OpenFlags::ReadOnly);
    token = mc::submit(mc::kSyscallEndpointOpen, &openArgs);
    if (token == 0 || !mc::wait(token) || openArgs.error != mc::ChannelError::None) {
        kFinish(1);
    }

    mc::uint64_t totalRead = 0;
    while (totalRead < statArgs.size) {
        mc::ReadArgs readArgs;
        readArgs.fd = openArgs.fd;
        readArgs.buf = gInitrdBuffer + totalRead;
        readArgs.len = static_cast<mc::uint32_t>(statArgs.size - totalRead);
        token = mc::submit(mc::kSyscallEndpointRead, &readArgs);
        if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None) {
            kFinish(1);
        }
        if (readArgs.bytesRead == 0) {
            break;  // EOF - statArgs.size와 불일치하면 아래에서 걸러진다.
        }
        totalRead += readArgs.bytesRead;
    }

    mc::CloseArgs closeArgs;
    closeArgs.fd = openArgs.fd;
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    if (token != 0) {
        mc::wait(token);
    }

    if (totalRead != statArgs.size) {
        kFinish(1);
    }

    // 2) cpio 아카이브에서 dbgtarget 엔트리 찾기.
    FindDbgTargetContext findCtx;
    cpio::forEachEntry(gInitrdBuffer, totalRead, kFindDbgTargetEntry, &findCtx);
    if (!findCtx.found) {
        kFinish(2);
    }

    // 3) SpawnProcess - kSpawnDebugStart로 첫 스레드가 이미 정지된
    //    채로 시작해 첫 명령 실행 전 시점을 놓치지 않는다.
    mc::SpawnProcessArgs spawnArgs;
    spawnArgs.imageBuffer = findCtx.data;
    spawnArgs.imageSize = findCtx.dataSize;
    spawnArgs.flags = mc::SpawnProcessFlags::kSpawnDebugStart;
    token = mc::submit(mc::kSyscallEndpointSpawnProcess, &spawnArgs);
    if (token == 0 || !mc::wait(token) || spawnArgs.error != mc::SpawnProcessError::None) {
        kFinish(3);
    }

    // 4) DebugAttach - 방금 스폰한 진짜 자식이므로 kFindDebuggableChild
    //    권한 검증을 통과한다.
    mc::DebugAttachArgs attachArgs;
    attachArgs.targetProcessId = spawnArgs.pid;
    token = mc::submit(mc::kSyscallEndpointDebugAttach, &attachArgs);
    if (token == 0 || !mc::wait(token) || attachArgs.error != mc::ChannelError::None) {
        kFinish(4);
    }

    // 5) 브레이크포인트 설정 - kBreakpointTarget 진입 지점(Execute).
    mc::DebugSetBreakpointArgs bpArgs;
    bpArgs.targetProcessId = spawnArgs.pid;
    bpArgs.slot = 0;
    bpArgs.address = kBreakpointTargetAddress;
    bpArgs.condition = mc::DebugBreakpoint::Condition::Execute;
    bpArgs.enable = true;
    token = mc::submit(mc::kSyscallEndpointDebugSetBreakpoint, &bpArgs);
    if (token == 0 || !mc::wait(token) || bpArgs.error != mc::ChannelError::None) {
        kFinish(5);
    }

    // 6) 최초 재개 - kSpawnDebugStart로 정지된 채 시작한 스레드0을 이제야
    //    처음 실행시킨다(dbgtarget/main.cpp의 _start()가 바로 이
    //    시점부터 CreateThread+kBreakpointTarget 루프를 시작한다).
    if (!kPollDebugContinue(spawnArgs.pid)) {
        kFinish(6);
    }

    // 7) 스트레스 루프 - PN-0556C759가 지목한 재현 절차 그대로: 두
    //    스레드가 각자 브레이크포인트에 도달하길 기다려(폴링) 레지스터를
    //    조회한 뒤 DebugContinue(all-stop -> continue-all)로 재개한다.
    mc::DebugRegisterSnapshot regs0;
    mc::DebugRegisterSnapshot regs1;
    for (mc::uint32_t round = 0; round < kStressRounds; ++round) {
        if (!kPollGetRegisters(spawnArgs.pid, 0, regs0)) {
            kFinish(7);
        }
        if (!kPollGetRegisters(spawnArgs.pid, 1, regs1)) {
            kFinish(7);
        }
        if (!kPollDebugContinue(spawnArgs.pid)) {
            kFinish(8);
        }
    }

    kFinish(0);
}
