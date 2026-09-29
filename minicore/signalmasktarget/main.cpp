// minicore/signalmasktarget - signalmasktest의 자식 프로세스 역할.
// 자기 자신의 시그널 마스크는 절대 건드리지 않는다 - 부모가
// SpawnProcess 전에 SignalMask(Block, SIGTERM)로 세팅해 둔 마스크를
// 그대로 물려받았는지(SP-0666DB3C §4.6 상속) 검증하는 게 이 바이너리의
// 존재 이유다.
//
// 시나리오: 부모가 이 프로세스를 스폰한 직후 Kill(this, Terminate)을
// 보낸다 - 마스크를 물려받았으면(부모가 스폰 전 마스크를 걸어 뒀을
// 때) 그 신호는 pendingSignals에 쌓이기만 하고 체크포인트가 건너뛰어
// 이 루프가 방해받지 않고 끝까지 돈 뒤 exitCode=42로 스스로 종료한다.
// 마스크를 안 물려받았으면(부모가 마스크를 안 걸었을 때, 대조군) 다음
// 체크포인트에서 커널이 강제 종료(exitCode=0 고정, RM-48E1E610 0번
// 참고)시켜 이 selfTerminate(42)까지 도달하지 못한다.
//
// 루프 횟수(kSpinIterations)는 부모가 Kill을 보낼 시간을 벌기 위한
// 여유 창 - kMaxRetriesForBootRace류 다른 테스트의 "통계적으로
// 충분히 큰 재시도 예산" 관례와 동일한 성격.
#include "libmc/process.h"
#include "libmc/syscall.h"

namespace {

constexpr mc::uint32_t kSpinIterations = 2000;

}  // namespace

extern "C" void _start() {
    for (mc::uint32_t i = 0; i < kSpinIterations; ++i) {
        // Wait(targetPid=-1)는 논블로킹이고 이 프로세스는 자식이 없어
        // 항상 즉시 반환한다(hadAnyChild=false) - 체크포인트를 거치는
        // 것 외엔 아무 부작용도 없는 순수 스핀 용도.
        mc::WaitArgs waitArgs;
        waitArgs.targetPid = -1;
        mc::SyscallToken token = mc::submit(mc::kSyscallEndpointWait, &waitArgs);
        if (token != 0) {
            mc::wait(token);
        }
    }
    mc::selfTerminate(42);
}
