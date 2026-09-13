#ifndef MINICORE_KERNEL_ACPI_H
#define MINICORE_KERNEL_ACPI_H

namespace kernel {

constexpr unsigned int kAcpiMaxCpus = 32;             // v1 임시 상한
constexpr unsigned int kAcpiMaxNumaNodes = 8;         // v1 임시 상한
constexpr unsigned int kAcpiMaxMemoryAffinityEntries = 32;
constexpr unsigned int kAcpiMaxIsoEntries = 16;       // ISA IRQ가 0-15뿐이라 이 이상 필요 없음
constexpr unsigned int kAcpiMaxIoApics = 8;           // v1 임시 상한 - 서버급 멀티소켓도 보통 이 안쪽

// Acpi::IsaIrqRouting::polarity/triggerMode 값 - ACPI MPS INTI Flags를
// "conforms(버스 기본값)"까지 전부 해석해 둔 최종 값이다.
constexpr unsigned int kAcpiPolarityActiveHigh = 1;
constexpr unsigned int kAcpiPolarityActiveLow = 3;
constexpr unsigned int kAcpiTriggerEdge = 1;
constexpr unsigned int kAcpiTriggerLevel = 3;

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

    // 시스템에 IOAPIC이 여러 개 있을 수 있다(설계자 지시, 2026-09-14 -
    // "IOAPIC는 한개가 아니라 여러개가 존재할 수도 있는데, 이 부분에
    // 대해서도 고려해야해") - 서버급 멀티소켓/멀티칩셋 구성에서 흔하다.
    // MADT의 IO APIC 엔트리(타입1)마다 각자 다른 MMIO 베이스 주소와
    // globalSystemInterruptBase(이 IOAPIC이 담당하는 GSI 범위의 시작)를
    // 갖는다 - GSI 하나를 실제로 라우팅하려면 어느 IOAPIC이 그 GSI를
    // 담당하는지부터 찾아야 한다(IoApic::setRedirection이 이 배열을
    // 순회해서 처리, ioapic.cpp 참고).
    static unsigned int ioApicCount();
    static unsigned int ioApicId(unsigned int index);
    static unsigned int ioApicAddress(unsigned int index);
    static unsigned int ioApicGsiBase(unsigned int index);

    // ISA IRQ(레거시 핀 번호, 0-15) 하나를 실제 라우팅할 GSI(Global
    // System Interrupt)/극성/트리거 모드로 바꾼다 - MADT Interrupt
    // Source Override(타입2)를 파싱한 결과다(QU-2EF510B4, 설계자 지시,
    // 2026-09-14 - "IRQ==GSI로 가정하지 말고 실제로 파싱해서 맵핑하라").
    // override가 없는 IRQ는 ISA 기본값(GSI=IRQ 그대로, active-high,
    // edge-triggered)으로 반환한다 - ACPI 스펙상 override 테이블에
    // 없는 ISA IRQ는 이 기본값을 따른다고 명시돼 있다.
    struct IsaIrqRouting {
        unsigned int gsi;
        // kPolarityActiveHigh/ActiveLow(아래 상수) - ACPI MPS INTI
        // Flags의 극성 필드를 ISA 기본값(active-high)까지 반영해
        // 이미 해석해 둔 값이다("conforms" 값은 여기 없음).
        unsigned int polarity;
        // kTriggerEdge/Level(아래 상수) - 위와 같은 이유로 이미 해석됨
        // (ISA 기본값 edge까지 반영).
        unsigned int triggerMode;
    };
    static IsaIrqRouting resolveIsaIrq(unsigned int isaIrq);

    // HPET("HPET" 시그니처)는 선택 테이블이다 - 없는 펌웨어도 흔하다.
    // hasHpet()이 false면 hpetAddress()는 의미 없다(호출부가 반드시
    // hasHpet()로 먼저 확인해야 함, DS-D4E5C451 - "HPET을 스케줄러
    // 기본 시간원으로, LAPIC 타이머는 fallback으로").
    static bool hasHpet();
    static unsigned long hpetAddress();

    // ACPI MCFG("MCFG" 시그니처) - PCIe MMCONFIG(ECAM) 베이스 주소
    // 테이블(QU-7B67E05A/QU-4C2DD71C, 설계자 지시, 2026-09-14 -
    // "MMCONFIG 및 0xCF8 둘 모두 고려하고 준비해야 Legacy fallback을
    // 구현할 수 있다"). 여러 PCI 세그먼트 그룹을 지원하는 시스템도
    // 있지만(서버급), 이 프로젝트는 첫 번째 엔트리(세그먼트 그룹 0)만
    // 다룬다 - 일반 데스크톱/서버 대부분이 세그먼트 그룹 0 하나뿐이라
    // 실용적인 범위(관계도에 기록).
    static bool hasMcfg();
    static unsigned long mcfgBaseAddress();
    static unsigned char mcfgStartBus();
    static unsigned char mcfgEndBus();

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
