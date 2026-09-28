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
//   15/16/17 = §6-C close-race 회귀 - 짧은 주기 타이머 생성/SetTime/
//     즉시 Close(Read 없이) 단계
//   18/19 = §6-C 후속 확인용 1회성 타이머 생성/SetTime
//   20/21/22 = §6-C 후속 확인 Read(20/21=submit/result 실패,
//     22=expirationCount==0) - close-race 이후에도 fd 테이블/타이머
//     인프라가 정상 동작하는지 확인(UAF/오염이 있었다면 여기서 크래시/
//     행업/이상 동작으로 드러난다)
//   23 = §6-C 후속 확인 Close 실패
//   24/25 = §6-A 절대시각 타이머 - 생성/SetTime(absolute=true, 이미
//     지난 유닉스 타임스탬프 - 즉시 만료 clamp 경로) 실패
//   26/27/28 = §6-A Read(26/27=submit/result 실패, 28=expirationCount==0)
//   29 = §6-A Close 실패
//   30/31 = §6-A "미래" 산술 경로 - 생성/SetTime(absolute=true, 먼
//     미래 타임스탬프) 실패(오버플로/스케줄 실패가 있다면 여기서 드러남)
//   32 = §6-A "미래" 경로 Close(취소) 실패
//
// §6-A 참고: 유저랜드에 아직 wall-clock 조회 API가 없어(userland/libs/
// libmc에 Rtc 거울 없음) "미래 목표 시각까지 정확히 기다리는지"는 이
// 테스트가 검증하지 못한다 - 대신 "이미 지난 절대 시각(유닉스
// 타임스탬프 1 = 1970-01-01 00:00:01)"을 줘서 커널의 clamp-to-0
// 경로(TimerfdSetTimeHandler, timerfd.cpp)가 크래시 없이 즉시
// 만료시키는지만 확인한다 - 환산 산술(목표-현재)*kSchedulerTickHz
// 자체는 간단해 코드 리뷰로 충분하다고 판단.
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

    // 3) SP-A7479F83 §6-C close-race 회귀 - 짧은 주기 타이머를 한 번도
    // Read하지 않은 채 즉시 Close() - cancel()이 성공하든(즉시 회수)
    // 실패하든(closing 플래그로 kOnTimerfdFire가 나중에 회수) 이후
    // 시스템이 여전히 정상 동작해야 한다(use-after-free/fd 테이블
    // 오염이 있었다면 아래 후속 단계에서 크래시/행업/이상 동작으로
    // 드러난다).
    mc::TimerfdCreateArgs raceCreateArgs;
    raceCreateArgs.periodic = true;
    token = mc::submit(mc::kSyscallEndpointTimerfdCreate, &raceCreateArgs);
    if (token == 0 || !mc::wait(token) || raceCreateArgs.error != mc::ChannelError::None) {
        kFinish(15);
    }
    const mc::int32_t raceFd = static_cast<mc::int32_t>(raceCreateArgs.fd);

    mc::TimerfdSetTimeArgs raceSetArgs;
    raceSetArgs.fd = raceFd;
    raceSetArgs.initialTicks = 5;
    raceSetArgs.intervalTicks = 5;
    token = mc::submit(mc::kSyscallEndpointTimerfdSetTime, &raceSetArgs);
    if (token == 0 || !mc::wait(token) || raceSetArgs.error != mc::ChannelError::None) {
        kFinish(16);
    }

    mc::CloseArgs raceCloseArgs;
    raceCloseArgs.fd = raceFd;
    token = mc::submit(mc::kSyscallEndpointClose, &raceCloseArgs);
    if (token == 0 || !mc::wait(token) || raceCloseArgs.error != mc::ChannelError::None) {
        kFinish(17);
    }

    // 원래 만료 시각(5~10틱 후)을 확실히 지나도록 30틱짜리 1회성
    // 타이머로 대기 - kOnTimerfdFire가 (혹시 늦게 실행되더라도)
    // closing 경로를 타고 지나갔을 시간을 확보한 뒤, fd 테이블/타이머
    // 인프라가 여전히 정상인지 끝까지 확인한다.
    mc::TimerfdCreateArgs waitCreateArgs;
    waitCreateArgs.periodic = false;
    token = mc::submit(mc::kSyscallEndpointTimerfdCreate, &waitCreateArgs);
    if (token == 0 || !mc::wait(token) || waitCreateArgs.error != mc::ChannelError::None) {
        kFinish(18);
    }
    const mc::int32_t waitFd = static_cast<mc::int32_t>(waitCreateArgs.fd);

    mc::TimerfdSetTimeArgs waitSetArgs;
    waitSetArgs.fd = waitFd;
    waitSetArgs.initialTicks = 30;
    waitSetArgs.intervalTicks = 0;
    token = mc::submit(mc::kSyscallEndpointTimerfdSetTime, &waitSetArgs);
    if (token == 0 || !mc::wait(token) || waitSetArgs.error != mc::ChannelError::None) {
        kFinish(19);
    }

    if (kReadExpirationCount(waitFd, 20, 21) == 0) {
        kFinish(22);
    }

    mc::CloseArgs waitCloseArgs;
    waitCloseArgs.fd = waitFd;
    token = mc::submit(mc::kSyscallEndpointClose, &waitCloseArgs);
    if (token == 0 || !mc::wait(token) || waitCloseArgs.error != mc::ChannelError::None) {
        kFinish(23);
    }

    // 4) SP-A7479F83 §6-A 절대시각 타이머 - 이미 지난 유닉스
    // 타임스탬프(1 = 1970-01-01 00:00:01)를 목표로 줘서 커널의
    // clamp-to-0 경로(TimerfdSetTimeHandler)가 크래시 없이 즉시
    // 만료시키는지 확인한다(파일 상단 주석 참고 - "미래" 산술은
    // 유저랜드에 wall-clock 조회 API가 없어 이 테스트로 검증 못함).
    mc::TimerfdCreateArgs absCreateArgs;
    absCreateArgs.periodic = false;
    token = mc::submit(mc::kSyscallEndpointTimerfdCreate, &absCreateArgs);
    if (token == 0 || !mc::wait(token) || absCreateArgs.error != mc::ChannelError::None) {
        kFinish(24);
    }
    const mc::int32_t absFd = static_cast<mc::int32_t>(absCreateArgs.fd);

    mc::TimerfdSetTimeArgs absSetArgs;
    absSetArgs.fd = absFd;
    absSetArgs.absolute = true;
    absSetArgs.initialTicks = 1;  // 유닉스 타임스탬프 1초 - 확실히 과거
    absSetArgs.intervalTicks = 0;
    token = mc::submit(mc::kSyscallEndpointTimerfdSetTime, &absSetArgs);
    if (token == 0 || !mc::wait(token) || absSetArgs.error != mc::ChannelError::None) {
        kFinish(25);
    }

    if (kReadExpirationCount(absFd, 26, 27) == 0) {
        kFinish(28);
    }

    mc::CloseArgs absCloseArgs;
    absCloseArgs.fd = absFd;
    token = mc::submit(mc::kSyscallEndpointClose, &absCloseArgs);
    if (token == 0 || !mc::wait(token) || absCloseArgs.error != mc::ChannelError::None) {
        kFinish(29);
    }

    // 5) §6-A "미래" 산술 경로 - 먼 미래 유닉스 타임스탬프(4102444800 =
    // 2100-01-01)를 줘서 (목표-현재)*kSchedulerTickHz 계산이 오버플로
    // 없이 정상적으로 스케줄되는지 확인한다(실제로 그 시각까지 기다릴
    // 수는 없으므로 스케줄 성공 직후 바로 Close()해 취소 - 3)의
    // close-race 경로와 달리 이 시점엔 활성 토큰이 확실히 아직 만료
    // 전이라 cancel()이 항상 true를 반환해야 정상이다).
    mc::TimerfdCreateArgs futureCreateArgs;
    futureCreateArgs.periodic = false;
    token = mc::submit(mc::kSyscallEndpointTimerfdCreate, &futureCreateArgs);
    if (token == 0 || !mc::wait(token) || futureCreateArgs.error != mc::ChannelError::None) {
        kFinish(30);
    }
    const mc::int32_t futureFd = static_cast<mc::int32_t>(futureCreateArgs.fd);

    mc::TimerfdSetTimeArgs futureSetArgs;
    futureSetArgs.fd = futureFd;
    futureSetArgs.absolute = true;
    futureSetArgs.initialTicks = 4102444800ULL;  // 2100-01-01 00:00:00 UTC
    futureSetArgs.intervalTicks = 0;
    token = mc::submit(mc::kSyscallEndpointTimerfdSetTime, &futureSetArgs);
    if (token == 0 || !mc::wait(token) || futureSetArgs.error != mc::ChannelError::None) {
        kFinish(31);
    }

    mc::CloseArgs futureCloseArgs;
    futureCloseArgs.fd = futureFd;
    token = mc::submit(mc::kSyscallEndpointClose, &futureCloseArgs);
    if (token == 0 || !mc::wait(token) || futureCloseArgs.error != mc::ChannelError::None) {
        kFinish(32);
    }

    kFinish(0);
}
