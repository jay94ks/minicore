#ifndef MINICORE_KERNEL_NMI_H
#define MINICORE_KERNEL_NMI_H

#include "libkenv/types.h"

namespace kernel {

// [PN-F443FE73, SP-677210E6 "NMI 활용 - 워치독 및 디버그 강제 정지
// IPI"] NMI(IST2, `kNmiVector=2`)는 마스크 불가능(cli로도 못 막음)
// 하다 - 이 성질을 이용한 코어 간 긴급 통지 두 가지: 워치독
// (Scheduler::onTick의 하트비트가 멈춘 코어를 진단), 디버그 강제
// 정지("stop the world" - kPanic 진입 시 나머지 코어 전부를 즉시
// 정지시켜 그 이후 공유 상태가 계속 바뀌며 진단 스냅샷이 오염되는
// 것을 막는다).
//
// NMI 자체는 페이로드가 없다(항상 벡터 2로만 전달) - TLB 샷다운의
// gPendingMask와 동일한 발상으로, 발신자가 보내기 직전 코어별 사유
// 슬롯(nmi.cpp의 파일 스코프 배열)을 먼저 채워 수신측 ISR이 구분하게
// 한다.
enum class NmiReason : uint32_t { None = 0, DebugHalt = 1, WatchdogTrap = 2 };

class Nmi {
public:
    // targetCoreIndex에게 NMI를 보내기 전 그 코어의 사유 슬롯부터
    // 채운다 - 두 단계(슬롯 세팅 -> IPI 발사) 사이에 다른 코어가
    // 끼어들 걱정이 없다(각 코어는 자기 슬롯만 쓰고, NMI가 도착하기
    // 전까지 그 슬롯을 읽는 사람이 없다).
    static void send(uint32_t targetCoreIndex, NmiReason reason);

    // idt.cpp의 NMI 처리 경로(kPanic(InterruptFrame*))가 자기 코어의
    // 사유를 읽어갈 때 쓴다.
    static NmiReason reasonForThisCore();

    // kPanic 두 진입점(panic.cpp의 kPanic(const char*), idt.cpp의
    // kPanic(InterruptFrame*)) 전용 - 자기 자신을 제외한 온라인 코어
    // 전부에게 DebugHalt를 보낸다("stop the world"). 호출부가 먼저
    // Lapic::isReady()를 확인해야 한다(LAPIC/ACPI가 아직 준비되지
    // 않은 극초반 부팅 중 패닉에서는 호출하지 않는다).
    static void stopAllOtherCores();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_NMI_H
