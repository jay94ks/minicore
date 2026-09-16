#include "panic.h"

#include "lapic.h"
#include "logger.h"
#include "nmi.h"

namespace kernel {

void kPanic(const char* message) {
    // [PN-F443FE73, SP-677210E6 "디버그 강제 정지(stop the world)"]
    // 다른 로그를 찍기 전에 최대한 빨리 - 나머지 온라인 코어가 공유
    // 상태를 계속 바꾸며 진단 스냅샷을 오염시키는 것을 막는다.
    // Lapic::isReady() 확인은 극초반(ACPI/LAPIC 준비 전) 패닉에서
    // Nmi::stopAllOtherCores()가 아직 존재하지 않는 LAPIC/코어 정보에
    // 기대는 것을 막기 위함이다.
    if (Lapic::isReady()) {
        Nmi::stopAllOtherCores();
    }
    Logger::panic("\nminicore: PANIC - %s", message);
    for (;;) {
        asm volatile("cli; hlt");
    }
}

}  // namespace kernel
