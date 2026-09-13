#ifndef MINICORE_KERNEL_IDT_H
#define MINICORE_KERNEL_IDT_H

#include "interrupt_frame.h"

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

    // vector: 33-254 범위(0-32, 0xFF는 커널이 이미 쓰고 있어 등록
    // 불가 - registerHandler가 그 범위를 받으면 그냥 무시한다).
    // handler는 EOI를 직접 보내지 않는다 - 반환 후 kIsrHandler가
    // Lapic::sendEoi()를 대신 호출한다(장치 핸들러마다 EOI를 빼먹는
    // 실수를 구조적으로 막기 위함).
    using InterruptHandler = void (*)(InterruptFrame*);
    static void registerHandler(unsigned int vector, InterruptHandler handler);
    static void unregisterHandler(unsigned int vector);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_IDT_H
