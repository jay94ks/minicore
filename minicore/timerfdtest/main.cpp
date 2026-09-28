// minicore/timerfdtest - PN-96265AE4/PN-0F56DE4B(timerfd) 실측 검증
// 전용 프로그램 - dbgtarget/proctest와 동일한 지위("정상적인 커널
// 서비스가 아니다, 부팅 매니페스트에 절대 등록하지 않는다", TEMP
// kmain.cpp 훅으로만 자동 실행).
//
// 시나리오: 1회성 타이머(20틱)→블로킹 Read 1회→Close, 그 다음 주기
// 타이머(초기15+주기15)→블로킹 Read 2회(PN-96265AE4가 발견한 "첫
// 블로킹 Read가 영원히 안 깨어남" 버그의 재현/회귀 검증 지점)→Close.
// MINICORE_QEMU_SMP=4 이상으로 돌려야 한다(PN-0F56DE4B가 이미 확인한
// DelayedExecutionQueue의 "idle 시에만 전진" 특성 - SMP=1이면 이
// 테스트 자신의 유저 스레드가 계속 실행 중이라 코어가 안 쉬어서
// 타이머가 아예 안 울릴 수 있다).
//
// exitCode 규약:
//   0 = 전 구간 정상
//   1 = 1회성 TimerfdCreate 실패
//   2 = 1회성 TimerfdSetTime 실패
//   3 = 1회성 Read 실패(submit/wait)
//   4 = 1회성 Read error!=None 이거나 bytesRead!=8
//   5 = 1회성 expirationCount==0(비정상 - 블로킹에서 깨어났는데 만료가 없음)
//   6 = 1회성 Close 실패
//   7 = 주기 TimerfdCreate 실패
//   8 = 주기 TimerfdSetTime 실패
//   9 = 주기 Read(1회차) 실패(submit/wait) - PN-96265AE4의 원래 hang 지점
//   10 = 주기 Read(1회차) error!=None 이거나 bytesRead!=8
//   11 = 주기 Read(1회차) expirationCount==0
//   12 = 주기 Read(2회차) 실패(submit/wait)
//   13 = 주기 Read(2회차) error!=None 이거나 bytesRead!=8
//   14 = 주기 Close 실패
#include "libmc/syscall.h"
#include "libmc/timerfd.h"
#include "libmc/vfs.h"

namespace {

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

mc::uint64_t kReadExpirationCount(mc::int32_t fd, mc::int32_t failSubmitCode, mc::int32_t failResultCode) {
    mc::uint64_t count = 0;
    mc::ReadArgs readArgs;
    readArgs.fd = fd;
    readArgs.buf = &count;
    readArgs.len = sizeof(count);
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointRead, &readArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(failSubmitCode);
    }
    if (readArgs.error != mc::ChannelError::None || readArgs.bytesRead != sizeof(count)) {
        kFinish(failResultCode);
    }
    return count;
}

}  // namespace

extern "C" void _start() {
    // 1) 1회성 타이머(20틱) - PN-395F4D89가 해소한 경로, 회귀 확인용.
    mc::TimerfdCreateArgs createArgs;
    createArgs.periodic = false;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointTimerfdCreate, &createArgs);
    if (token == 0 || !mc::wait(token) || createArgs.error != mc::ChannelError::None) {
        kFinish(1);
    }
    const mc::int32_t oneshotFd = static_cast<mc::int32_t>(createArgs.fd);

    mc::TimerfdSetTimeArgs setArgs;
    setArgs.fd = oneshotFd;
    setArgs.initialTicks = 20;
    setArgs.intervalTicks = 0;
    token = mc::submit(mc::kSyscallEndpointTimerfdSetTime, &setArgs);
    if (token == 0 || !mc::wait(token) || setArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }

    if (kReadExpirationCount(oneshotFd, 3, 4) == 0) {
        kFinish(5);
    }

    mc::CloseArgs closeArgs;
    closeArgs.fd = oneshotFd;
    token = mc::submit(mc::kSyscallEndpointClose, &closeArgs);
    if (token == 0 || !mc::wait(token) || closeArgs.error != mc::ChannelError::None) {
        kFinish(6);
    }

    // 2) 주기 타이머(초기15+주기15) - PN-96265AE4가 발견한 hang의
    //    핵심 재현/회귀 지점(첫 블로킹 Read).
    mc::TimerfdCreateArgs periodicCreateArgs;
    periodicCreateArgs.periodic = true;
    token = mc::submit(mc::kSyscallEndpointTimerfdCreate, &periodicCreateArgs);
    if (token == 0 || !mc::wait(token) || periodicCreateArgs.error != mc::ChannelError::None) {
        kFinish(7);
    }
    const mc::int32_t periodicFd = static_cast<mc::int32_t>(periodicCreateArgs.fd);

    mc::TimerfdSetTimeArgs periodicSetArgs;
    periodicSetArgs.fd = periodicFd;
    periodicSetArgs.initialTicks = 15;
    periodicSetArgs.intervalTicks = 15;
    token = mc::submit(mc::kSyscallEndpointTimerfdSetTime, &periodicSetArgs);
    if (token == 0 || !mc::wait(token) || periodicSetArgs.error != mc::ChannelError::None) {
        kFinish(8);
    }

    if (kReadExpirationCount(periodicFd, 9, 10) == 0) {
        kFinish(11);
    }
    if (kReadExpirationCount(periodicFd, 12, 13) == 0) {
        kFinish(13);
    }

    mc::CloseArgs periodicCloseArgs;
    periodicCloseArgs.fd = periodicFd;
    token = mc::submit(mc::kSyscallEndpointClose, &periodicCloseArgs);
    if (token == 0 || !mc::wait(token) || periodicCloseArgs.error != mc::ChannelError::None) {
        kFinish(14);
    }

    kFinish(0);
}
