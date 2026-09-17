#include "panic.h"

#include "lapic.h"
#include "libkenv/spinlock.h"
#include "logger.h"
#include "nmi.h"

namespace {

// [PN-3081704A] "최초 1회만" 래치 본체 - 0(미청구)에서 1로 성공적으로
// CAS한 코어만 진짜 패닉 시퀀스를 진행한다. 파일 스코프 전역이라
// panic.cpp/idt.cpp 양쪽에서 kTryClaimFirstPanic()을 통해서만
// 접근한다(직접 노출 안 함 - 두 kPanic() 진입점 외에는 쓸 이유가
// 없다).
kernel::Atomic<kernel::uint32_t> gPanicClaimed{0};

}  // namespace

namespace kernel {

bool kTryClaimFirstPanic() {
    uint32_t expected = 0;
    return gPanicClaimed.compareExchange(expected, 1);
}

void kPanic(const char* message) {
    if (!kTryClaimFirstPanic()) {
        // [PN-3081704A] 이미 다른 코어가 먼저 패닉 중이다 - 그 코어가
        // 곧 보낼(또는 이미 보낸) stop-the-world NMI가 나를 정지시킬
        // 것이므로 stopAllOtherCores()/로그 출력을 또 하지 않는다.
        // 여기서 바로 cli를 걸어 두는 이유는, 이 지점 이후로 내가 또
        // 다른 인터럽트(예: 워치독 NMI)에 걸려 이 정지 시퀀스 자체가
        // 늘어지는 것을 막기 위함 - 실제 NMI 자체는 cli로도 못 막지만,
        // 적어도 자발적으로 더 할 일을 만들지 않는다.
        asm volatile("cli");
        for (;;) {
            asm volatile("hlt");
        }
    }

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
