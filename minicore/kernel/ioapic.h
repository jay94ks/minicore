#ifndef MINICORE_KERNEL_IOAPIC_H
#define MINICORE_KERNEL_IOAPIC_H

namespace kernel {

// ACPI MADT가 알려주는 IOAPIC(Acpi::ioApicAddress())을 매핑해 핀 기반
// 외부 인터럽트(키보드 등 레거시 IRQ)를 원하는 벡터로 라우팅한다
// (PL-2D149D8F). LAPIC과 달리 IOREGSEL(오프셋 0x00에 레지스터 번호를
// 써서 선택)+IOWIN(오프셋 0x10으로 실제 값 접근)의 2단계 간접 방식이다.
//
// **주의(범위 밖)**: ACPI MADT의 Interrupt Source Override(타입2)
// 엔트리를 아직 파싱하지 않는다(Acpi 클래스가 Local APIC/IO APIC
// 엔트리만 다룸) - 그래서 여기서는 "IRQ 번호 == GSI(Global System
// Interrupt) 번호"로 가정한다. 실제 하드웨어/일부 BIOS는 특히 IRQ0을
// 다른 GSI로 재배선해 두는 경우가 있다 - 지금은 GSI 재배선이 거의
// 없는 IRQ1(키보드)만 다뤄서 문제가 없지만, PIT(IRQ0) 등을 IOAPIC으로
// 라우팅해야 할 일이 생기면 Interrupt Source Override 파싱을 먼저
// 추가해야 한다.
class IoApic {
public:
    static void init();

    // irq: ISA IRQ 번호(위 주의사항 참고 - 지금은 GSI와 동일하다고
    // 가정). vector: IDT에 등록된 목적지 벡터(33-254, Idt::kDynamicVectorBase
    // 대역과 registerHandler로 실제 콜백을 먼저 걸어 둬야 함).
    // destApicId: 인터럽트를 받을 코어의(x)APIC ID - IOAPIC 리다이렉션
    // 테이블의 물리 목적지 필드는 8비트라 x2APIC의 32비트 ID 전체는
    // 못 담는다(지금은 BSP 하나뿐이라 문제 없음 - SMP 확장 시 재검토).
    static void setRedirection(unsigned int irq, unsigned int vector, unsigned int destApicId);
    static void mask(unsigned int irq);
    static void unmask(unsigned int irq);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_IOAPIC_H
