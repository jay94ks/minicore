// minicore/proctest - PN-012D6310(procfs pid 열람 실측 검증) 전용
// 최소 테스트 프로그램 - minicore/dbgtarget과 동일한 지위("정상적인
// 커널 서비스가 아니다, 부팅 매니페스트에 절대 등록하지 않는다") -
// 반드시 실제 SpawnProcess syscall로 스폰돼야 유효한 ProcessId를
// 받는다.
//
// 이 프로그램의 유일한 목적: "실제로 SpawnProcess를 거쳐 유효한
// ProcessId를 발급받은 프로세스가 자기 자신의 procfs status
// (`/sys/live/proc/self/status`)를 여닫을 수 있는가"를 실측한다 -
// PN-85FA4992가 설계한 self selector(processId 있으면 새 pid 인코딩
// 경로)의 유일한 실사용 검증 대상. 유저랜드에 콘솔/로그 출력 경로가
// 아직 없어(SP-DF89897F 미착수) 결과를 진단 문자열로 보고할 방법이
// 없다 - 대신 exitCode로 결과를 인코딩해, 이 프로세스를 스폰한
// 부모가 Wait()로 회수(reap)한 exitCode를 확인하는 방식으로 검증한다.
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kSelfStatusPath[] = "/sys/live/proc/self/status";

// exitCode 규약(부모가 Wait()의 reapedPid/exitCode로 확인):
//   0 = open 성공 + read 성공 + 내용이 "Pid:"로 시작(정상)
//   1 = open 실패(mc::submit이 토큰을 못 받았거나 mc::wait가 false)
//   2 = open은 성공했지만 error != None
//   3 = read 실패(submit/wait 실패)
//   4 = read는 성공했지만 error != None 이거나 bytesRead == 0
//   5 = 내용이 "Pid:"로 시작하지 않음(포맷 불일치)
[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

}  // namespace

extern "C" void _start() {
    mc::OpenArgs openArgs;
    openArgs.path = kSelfStatusPath;
    openArgs.pathLen = sizeof(kSelfStatusPath) - 1;
    openArgs.flags = static_cast<mc::uint32_t>(mc::OpenFlags::ReadOnly);

    mc::SyscallToken openToken = mc::submit(mc::kSyscallEndpointOpen, &openArgs);
    if (openToken == 0 || !mc::wait(openToken)) {
        kFinish(1);
    }
    if (openArgs.error != mc::ChannelError::None) {
        kFinish(2);
    }

    mc::ReadArgs readArgs;
    readArgs.fd = openArgs.fd;
    char buf[64] = {};
    readArgs.buf = buf;
    readArgs.len = sizeof(buf) - 1;

    mc::SyscallToken readToken = mc::submit(mc::kSyscallEndpointRead, &readArgs);
    if (readToken == 0 || !mc::wait(readToken)) {
        kFinish(3);
    }
    if (readArgs.error != mc::ChannelError::None || readArgs.bytesRead == 0) {
        kFinish(4);
    }

    constexpr char kExpectedPrefix[] = "Pid:";
    for (mc::uint32_t i = 0; i < sizeof(kExpectedPrefix) - 1; ++i) {
        if (buf[i] != kExpectedPrefix[i]) {
            kFinish(5);
        }
    }
    kFinish(0);
}
