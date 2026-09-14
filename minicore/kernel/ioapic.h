#ifndef MINICORE_KERNEL_IOAPIC_H
#define MINICORE_KERNEL_IOAPIC_H

#include "libkenv/types.h"

namespace kernel {

// ACPI MADT가 알려주는 IOAPIC들(Acpi::ioApicCount()/ioApicAddress())을
// 전부 매핑해 핀 기반 외부 인터럽트(키보드 등 레거시 IRQ)를 원하는
// 벡터로 라우팅한다(PL-2D149D8F). LAPIC과 달리 IOREGSEL(오프셋 0x00에
// 레지스터 번호를 써서 선택)+IOWIN(오프셋 0x10으로 실제 값 접근)의
// 2단계 간접 방식이다.
//
// 시스템에 IOAPIC이 여러 개 있을 수 있다(설계자 지시, 2026-09-14 -
// "IOAPIC는 한개가 아니라 여러개가 존재할 수도 있는데, 이 부분에
// 대해서도 고려해야해") - 각 IOAPIC은 ACPI가 알려주는
// globalSystemInterruptBase부터 시작하는 서로 겹치지 않는 GSI 구간을
// 담당한다. 이 클래스의 gsi 기반 API(setRedirection/mask/unmask)는
// 호출부가 어느 IOAPIC이 그 GSI를 담당하는지 몰라도 되게, 내부에서
// GSI 값으로 올바른 IOAPIC 인스턴스를 찾아 라우팅한다.
//
// ACPI MADT Interrupt Source Override(타입2)를 Acpi::resolveIsaIrq로
// 실제로 반영한다(QU-2EF510B4, 설계자 지시, 2026-09-14 - "IRQ==GSI로
// 가정하지 말고 실제로 파싱해서 맵핑하라") - setRedirectionForIsaIrq가
// 이 변환을 대신해 준다.
class IoApic {
public:
    static void init();

    // gsi: Global System Interrupt(IOAPIC 리다이렉션 테이블 인덱스 그
    // 자체) - HPET처럼 이미 GSI 단위로 라우팅을 정하는 경우 직접
    // 쓴다. polarity/triggerMode는 kAcpiPolarityActiveHigh/ActiveLow,
    // kAcpiTriggerEdge/Level(acpi.h) 값.
    // destApicId: 인터럽트를 받을 코어의 (x)APIC ID - 32비트 타입으로
    // 관리하되(QU-F89355A8, 설계자 지시 - "32bit 타입으로 관리하고,
    // 8비트만 사용가능한 시스템에서 32비트 중 8비트를 활용"), IOAPIC
    // REDTBL의 물리 목적지 필드 자체는 하드웨어 스펙상 8비트라
    // 0xFF를 넘는 목적지는 애초에 라우팅이 불가능하다 - 그 경우 false를
    // 반환한다(조용히 잘못된 대상으로 보내지 않기 위함, 그런 코어는
    // MSI/x2APIC 논리주소 등 다른 전달 경로를 써야 한다).
    // gsi를 담당하는 IOAPIC이 없거나(어느 인스턴스의 [gsiBase,
    // gsiBase+최대 리다이렉션 엔트리 수) 구간에도 안 들어감), 담당
    // IOAPIC은 찾았지만 그 안에서 지역 인덱스가 IOAPICVER가 알려주는
    // 최대 엔트리 수를 넘어도 false.
    static bool setRedirection(uint32_t gsi, uint32_t vector, uint32_t destApicId,
                                uint32_t polarity, uint32_t triggerMode);

    // ISA IRQ(레거시 핀 번호, 0-15) 하나를 라우팅하는 편의 함수 -
    // Acpi::resolveIsaIrq로 실제 GSI/극성/트리거를 구해 위
    // setRedirection을 호출한다.
    static bool setRedirectionForIsaIrq(uint32_t isaIrq, uint32_t vector, uint32_t destApicId);

    static void mask(uint32_t gsi);
    static void unmask(uint32_t gsi);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_IOAPIC_H
