#ifndef MINICORE_KERNEL_IDT_H
#define MINICORE_KERNEL_IDT_H

#include "interrupt_frame.h"
#include "libkenv/types.h"

namespace kernel {

// CPU 예외(벡터 0-31) + 타이머(32)/spurious(0xFF)는 커널 자신이
// 고정 처리하고, 그 사이(33-254)는 IOAPIC/(추후) MSI가 임의로 라우팅할
// 수 있는 범용 벡터로 비워 뒀다(isr.S가 전부 스텁을 미리 찍어냄,
// PL-2D149D8F). 이 범용 벡터는 registerHandler로 실제 장치 핸들러를
// 등록해야 동작한다 - 등록 안 된 벡터로 인터럽트가 들어오면 진단
// 로그를 남기고 멈춘다(라우팅 설정 버그를 조용히 무시하지 않기
// 위함).
class Idt {
public:
    static void init();

    // IDT 내용(gIdt)은 전역 하나뿐이라 다시 만들 필요가 없지만,
    // IDTR은 코어마다 별도 레지스터라 각 코어가 자기 몫으로 lidt를
    // 한 번씩 실행해야 한다(SMP AP 기동, PL-65C20380) - init()을
    // 다시 부르면 이미 켜진 다른 코어가 쓰는 것과 동일한 내용을
    // 다시 써도 안전하긴 하지만(결정적 내용이라 값 자체는 그대로),
    // 이 메서드는 그 재작성 없이 lidt만 실행해 의도를 더 분명히 한다.
    static void reloadOnThisCore();

    // vector: 33-254 범위(0-32, 0xFF는 커널이 이미 쓰고 있어 등록
    // 불가 - registerHandler가 그 범위를 받으면 그냥 무시한다).
    // handler는 EOI를 직접 보내지 않는다 - 반환 후 kIsrHandler가
    // Lapic::sendEoi()를 대신 호출한다(장치 핸들러마다 EOI를 빼먹는
    // 실수를 구조적으로 막기 위함).
    using InterruptHandler = void (*)(InterruptFrame*);
    static void registerHandler(uint32_t vector, InterruptHandler handler);
    static void unregisterHandler(uint32_t vector);

    // [PN-F443FE73, SP-677210E6 "#DB(Debug) 상세 설계"] #DB(벡터1)는
    // 고정 CPU 예외(0-31)라 위 registerHandler(33-254 전용)를 못 쓴다 -
    // 커널 내부에서 #DB를 소비하고 싶은 쪽(디버거 서브시스템 등)이
    // 등록하는 전용 슬롯. 콜백이 true를 반환하면 "내가 처리했다"는
    // 뜻(필요한 상태 조작을 이미 끝냈다는 전제로 그냥 iretq) - false면
    // (또는 콜백 미등록이면) 로그만 남기고 계속 실행한다(#MC/NMI와
    // 달리 #DB 미처리는 오류가 아니다 - 브레이크포인트/싱글스텝은
    // 의도적 이벤트).
    using DebugCallback = bool (*)(InterruptFrame*, uint64_t dr6);
    static void registerDebugCallback(DebugCallback callback);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_IDT_H
