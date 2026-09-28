// minicore/exittest - PN-5EDE3C96 항목1(Process::exitCode 배선) 실측
// 검증용 자식 프로그램 - dbgtarget/proctest와 동일한 지위("정상적인
// 커널 서비스가 아니다, 부팅 매니페스트에 절대 등록하지 않는다") -
// minicore/exitwaiter가 실제 SpawnProcess syscall로 이 프로그램을
// 스폰하고 Wait()으로 회수해 exitCode가 정확히 전달됐는지 확인한다.
//
// 하는 일은 이것뿐: 즉시 mc::selfTerminate(kExpectedExitCode)를 호출.
#include "libmc/syscall.h"

namespace {
constexpr mc::int32_t kExpectedExitCode = 77;
}  // namespace

extern "C" void _start() { mc::selfTerminate(kExpectedExitCode); }
