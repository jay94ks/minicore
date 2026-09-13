#ifndef MINICORE_KERNEL_ACPI_H
#define MINICORE_KERNEL_ACPI_H

namespace kernel {

constexpr unsigned int kAcpiMaxCpus = 32;             // v1 임시 상한
constexpr unsigned int kAcpiMaxNumaNodes = 8;         // v1 임시 상한
constexpr unsigned int kAcpiMaxMemoryAffinityEntries = 32;

// RSDP -> RSDT/XSDT -> MADT("APIC")/SRAT까지 파싱해서 로컬 APIC 주소,
// 코어(Local APIC) 목록, 그리고 **NUMA 노드 토폴로지**(설계자 지시,
// 2026-09-14 - "ACPI를 파싱하고 노드 토폴로지를 확실히 파악하고
// 멀티노드 구조를 확립해야 한다")까지 알아낸다.
//
// SRAT는 선택 테이블이라(진짜 NUMA 머신에만 있음) 없으면 전체를
// 노드 0 하나로 취급하는 fallback으로 돌아간다 - 이게 QEMU 기본
// 환경(비-NUMA)의 정상 동작이다.
//
// 실제 AP(나머지 코어) 기동(트램폴린 + INIT-SIPI-SIPI)은 아직 이
// 클래스의 범위 밖이다(다음 마일스톤).
class Acpi {
public:
    // rsdpPhys: hvm_start_info.rsdpPaddr. Paging::init()(direct map)
    // 이후에 호출해야 한다 - 테이블을 kPhysToVirt로 읽는다.
    static bool init(unsigned long rsdpPhys);

    static unsigned long localApicAddress();
    static unsigned int cpuCount();
    static unsigned int cpuApicId(unsigned int index);
    static unsigned int ioApicAddress();

    // HPET("HPET" 시그니처)는 선택 테이블이다 - 없는 펌웨어도 흔하다.
    // hasHpet()이 false면 hpetAddress()는 의미 없다(호출부가 반드시
    // hasHpet()로 먼저 확인해야 함, DS-D4E5C451 - "HPET을 스케줄러
    // 기본 시간원으로, LAPIC 타이머는 fallback으로").
    static bool hasHpet();
    static unsigned long hpetAddress();

    // NUMA 토폴로지 (SRAT 없으면 항상 numaNodeCount()==1로 fallback).
    static unsigned int numaNodeCount();
    // cpuApicId(index)와 같은 index로 대응되는 코어가 속한 노드.
    static unsigned int cpuNumaNode(unsigned int index);

    // 물리 메모리 범위별 소속 노드 - PageFrameAllocator가 usable
    // 메모리를 노드별로 나누는 데 쓴다. SRAT가 없으면 개수 0을
    // 반환한다(호출부는 "정보 없음 = 전부 노드 0"으로 취급해야 함).
    static unsigned int memoryAffinityCount();
    static unsigned long memoryAffinityBase(unsigned int index);
    static unsigned long memoryAffinityLength(unsigned int index);
    static unsigned int memoryAffinityNode(unsigned int index);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_ACPI_H
