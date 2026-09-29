// minicore/rtsignalchild - rtsignaltest의 자식 프로세스 역할. RT 신호
// (SIGRT0=32)만 감시하는 signalfd를 열고 Read를 3번 반복 호출한다 -
// 표준 신호와 달리 같은 번호가 여러 번 와도 전부 별도 인스턴스로
// FIFO 순서대로 반환돼야 한다(SP-A7479F83 §6-B 핵심 차이) - 부모가
// 보낸 userData(100/200/300)가 정확히 그 순서로 돌아오는지까지
// 확인한다.
//
// exitCode: 1=SignalfdCreate 실패, 2/4/6=1·2·3번째 Read submit/wait
// 실패, 3/5/7=1·2·3번째 Read error!=None, 8=1번째 signo!=32,
// 9=1번째 userData!=100, 10=2번째 signo!=32, 11=2번째 userData!=200,
// 12=3번째 signo!=32, 13=3번째 userData!=300, **66=전체 성공**(0이
// 아닌 이유는 다른 테스트들과 동일 - 0은 신호 강제종료의 고정값).
#include "libmc/signal.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr mc::uint32_t kRtSignal0 = mc::kRtSignalBase;  // SIGRT0 = 32

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

mc::SignalfdSiginfo kReadOne(mc::int32_t fd, mc::int32_t failSubmitCode, mc::int32_t failErrorCode) {
    mc::SignalfdSiginfo info;
    mc::ReadArgs readArgs;
    readArgs.fd = fd;
    readArgs.buf = &info;
    readArgs.len = sizeof(info);
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointRead, &readArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(failSubmitCode);
    }
    if (readArgs.error != mc::ChannelError::None || readArgs.bytesRead != sizeof(info)) {
        kFinish(failErrorCode);
    }
    return info;
}

}  // namespace

extern "C" void _start() {
    mc::SignalfdCreateArgs createArgs;
    createArgs.signalMask = 0;
    createArgs.rtSignalMask = 1u << (kRtSignal0 - mc::kRtSignalBase);  // SIGRT0만 감시
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSignalfdCreate, &createArgs);
    if (token == 0 || !mc::wait(token) || createArgs.error != mc::ChannelError::None) {
        kFinish(1);
    }
    const mc::int32_t fd = static_cast<mc::int32_t>(createArgs.fd);

    const mc::SignalfdSiginfo first = kReadOne(fd, 2, 3);
    if (first.signo != kRtSignal0) {
        kFinish(8);
    }
    if (first.userData != 100) {
        kFinish(9);
    }

    const mc::SignalfdSiginfo second = kReadOne(fd, 4, 5);
    if (second.signo != kRtSignal0) {
        kFinish(10);
    }
    if (second.userData != 200) {
        kFinish(11);
    }

    const mc::SignalfdSiginfo third = kReadOne(fd, 6, 7);
    if (third.signo != kRtSignal0) {
        kFinish(12);
    }
    if (third.userData != 300) {
        kFinish(13);
    }

    kFinish(66);
}
