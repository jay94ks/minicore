// minicore/dbgtarget - PN-87D6B615(프로세스 디버깅 서브시스템) "남은
// 범위" 2번(실제 initrd 기반 유저 프로세스로 브레이크포인트/싱글스텝
// hit->정지->재개 왕복 실측)을 위한 전용 최소 디버기 프로그램 - 오래
// 살아있는 루프 + 브레이크포인트를 걸 명확한 지점(kBreakpointTarget,
// noinline) 하나만 제공한다.
//
// **정상적인 커널 서비스가 아니다 - 부팅 매니페스트(kmain.cpp의
// gServiceManifest)에 절대 등록하지 않는다**: 그 경로로 스폰되는
// 프로세스는 전부 parent=nullptr(커널이 직접 execImage()로 스폰)라
// DebugAttach의 "직계 부모" 권한 검증(debug_session.cpp
// kFindDebuggableChild)을 원천적으로 통과할 수 없다 - 반드시 실제
// SpawnProcess syscall을 통해, 그 호출자를 부모로 삼아 스폰돼야
// 한다(PN-87D6B615 "남은 범위 2번 실현 가능성 조사" 절 참고).
#include "libmc/process.h"
#include "libmc/syscall.h"

namespace {

// noinline - 브레이크포인트 주소가 objdump/nm으로 명확히 식별
// 가능한 별도 심볼이어야 한다(PN-87D6B615 권장 착수 순서 4번).
__attribute__((noinline)) void kBreakpointTarget(mc::uint64_t iteration) {
    // 컴파일러가 이 함수를 통째로 최적화해 없애지 못하도록 인라인
    // asm으로 값을 "쓰는 척"만 한다 - 실제 부작용은 없다.
    asm volatile("" : : "r"(iteration) : "memory");
}

// [신규, PN-0556C759 재현 인프라] 멀티스레드 동시 하드웨어 브레이크
// 포인트 경쟁을 재현하려면 같은 kBreakpointTarget을 반복 호출하는
// 두 번째 스레드가 필요하다(계획 본문 "재현 방법" 절) - CreateThread
// (RDI=arg 하나만 받는 진입점 관례, process.h 참고)로 추가한다. 이
// 두 번째 스레드는 dbgtarget 자체와 마찬가지로 영구 유지 - 재현
// 조사를 재개할 다음 세션도 그대로 재사용한다.
[[noreturn]] void kSecondThreadEntry(mc::uint64_t) {
    for (mc::uint64_t i = 0;; ++i) {
        kBreakpointTarget(i);
    }
}

}  // namespace

extern "C" void _start() {
    mc::CreateThreadArgs createArgs;
    createArgs.entry = reinterpret_cast<mc::uint64_t>(&kSecondThreadEntry);
    createArgs.arg = 0;
    mc::SyscallToken createToken = mc::submit(mc::kSyscallEndpointCreateThread, &createArgs);
    // CreateThread 실패해도(예: CreateThread syscall 자체가 아직
    // 등록되지 않은 커널 빌드) 원래의 단일 스레드 동작으로는 계속
    // 진행한다 - 두 번째 스레드는 이 프로그램의 필수 기능이 아니라
    // PN-0556C759류 멀티스레드 재현 시나리오를 위한 확장이다.
    if (createToken != 0) {
        mc::wait(createToken);
    }

    for (mc::uint64_t i = 0;; ++i) {
        kBreakpointTarget(i);
    }
}
