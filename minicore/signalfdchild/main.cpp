// minicore/signalfdchild - signalfdtest의 자식 프로세스 역할. 부모가
// SpawnProcess 전에 SignalMask(Block, SIGTERM)를 걸어 둬(상속됨,
// SP-0666DB3C §4.6) 이 프로세스가 태어나는 순간부터 이미 SIGTERM이
// 블록된 상태다 - 그 위에서 SignalfdCreate(SIGTERM)로 signalfd를 열고
// 부모의 Kill(this, Terminate)이 이 fd의 펜딩 큐로 도착하는지 Read로
// 확인한다(일반 종료 경로로 새면 이 프로세스는 selfTerminate에
// 도달하지 못하고 커널이 강제 종료시킨다 - exitCode=0 고정).
//
// exitCode: 1=SignalfdCreate submit/wait 실패, 2=SignalfdCreate
// error!=None, 3=Read submit/wait 실패, 4=Read error!=None,
// 5=bytesRead 불일치, 6=signo!=Terminate, **55=전체 성공**(0이 아닌
// 이유는 signalmasktest와 동일 - 0은 신호 강제종료의 고정값과 겹침).
#include "libmc/signal.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

extern "C" void _start() {
    mc::SignalfdCreateArgs createArgs;
    createArgs.signalMask = 1u << static_cast<mc::uint32_t>(mc::SignalNumber::Terminate);
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSignalfdCreate, &createArgs);
    if (token == 0 || !mc::wait(token)) {
        mc::selfTerminate(1);
    }
    if (createArgs.error != mc::ChannelError::None) {
        mc::selfTerminate(2);
    }

    mc::SignalfdSiginfo info;
    mc::ReadArgs readArgs;
    readArgs.fd = static_cast<mc::int32_t>(createArgs.fd);
    readArgs.buf = &info;
    readArgs.len = sizeof(info);
    token = mc::submit(mc::kSyscallEndpointRead, &readArgs);
    if (token == 0 || !mc::wait(token)) {
        mc::selfTerminate(3);
    }
    if (readArgs.error != mc::ChannelError::None) {
        mc::selfTerminate(4);
    }
    if (readArgs.bytesRead != sizeof(info)) {
        mc::selfTerminate(5);
    }
    if (info.signo != static_cast<mc::uint32_t>(mc::SignalNumber::Terminate)) {
        mc::selfTerminate(6);
    }

    mc::selfTerminate(55);
}
