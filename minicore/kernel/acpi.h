#ifndef MINICORE_KERNEL_ACPI_H
#define MINICORE_KERNEL_ACPI_H

namespace kernel {

constexpr unsigned int kAcpiMaxCpus = 32;  // v1 임시 상한 - 그 이상은 무시하고 로그만 남긴다

// RSDP -> RSDT/XSDT -> MADT("APIC")까지 파싱해서 로컬 APIC 주소와
// 코어(Local APIC) 목록을 알아낸다. SMP 부팅(설계자 지시, 2026-09-14)
// 을 하려면 몇 개의 코어가 있고 그 APIC ID가 뭔지부터 알아야 하므로
// 이 시점에 필요하다 - 실제 AP(나머지 코어) 기동(트램폴린 + INIT-
// SIPI-SIPI)은 아직 이 클래스의 범위 밖이다(다음 마일스톤).
class Acpi {
public:
    // rsdpPhys: hvm_start_info.rsdpPaddr. Paging::init()(direct map)
    // 이후에 호출해야 한다 - 테이블을 kPhysToVirt로 읽는다.
    static bool init(unsigned long rsdpPhys);

    static unsigned long localApicAddress();
    static unsigned int cpuCount();
    static unsigned int cpuApicId(unsigned int index);
    static unsigned int ioApicAddress();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_ACPI_H
