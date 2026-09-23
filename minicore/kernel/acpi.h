#ifndef MINICORE_KERNEL_ACPI_H
#define MINICORE_KERNEL_ACPI_H

#include "libkenv/types.h"

namespace kernel {

constexpr uint32_t kAcpiMaxCpus = 32;             // v1 임시 상한
constexpr uint32_t kAcpiMaxNumaNodes = 8;         // v1 임시 상한
constexpr uint32_t kAcpiMaxMemoryAffinityEntries = 32;
constexpr uint32_t kAcpiMaxIsoEntries = 16;       // ISA IRQ가 0-15뿐이라 이 이상 필요 없음
constexpr uint32_t kAcpiMaxIoApics = 8;           // v1 임시 상한 - 서버급 멀티소켓도 보통 이 안쪽

// Acpi::IsaIrqRouting::polarity/triggerMode 값 - ACPI MPS INTI Flags를
// "conforms(버스 기본값)"까지 전부 해석해 둔 최종 값이다.
constexpr uint32_t kAcpiPolarityActiveHigh = 1;
constexpr uint32_t kAcpiPolarityActiveLow = 3;
constexpr uint32_t kAcpiTriggerEdge = 1;
constexpr uint32_t kAcpiTriggerLevel = 3;

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
    static bool init(uint64_t rsdpPhys);

    static uint64_t localApicAddress();
    static uint32_t cpuCount();
    static uint32_t cpuApicId(uint32_t index);

    // 시스템에 IOAPIC이 여러 개 있을 수 있다(설계자 지시, 2026-09-14 -
    // "IOAPIC는 한개가 아니라 여러개가 존재할 수도 있는데, 이 부분에
    // 대해서도 고려해야해") - 서버급 멀티소켓/멀티칩셋 구성에서 흔하다.
    // MADT의 IO APIC 엔트리(타입1)마다 각자 다른 MMIO 베이스 주소와
    // globalSystemInterruptBase(이 IOAPIC이 담당하는 GSI 범위의 시작)를
    // 갖는다 - GSI 하나를 실제로 라우팅하려면 어느 IOAPIC이 그 GSI를
    // 담당하는지부터 찾아야 한다(IoApic::setRedirection이 이 배열을
    // 순회해서 처리, ioapic.cpp 참고).
    static uint32_t ioApicCount();
    static uint32_t ioApicId(uint32_t index);
    static uint32_t ioApicAddress(uint32_t index);
    static uint32_t ioApicGsiBase(uint32_t index);

    // ISA IRQ(레거시 핀 번호, 0-15) 하나를 실제 라우팅할 GSI(Global
    // System Interrupt)/극성/트리거 모드로 바꾼다 - MADT Interrupt
    // Source Override(타입2)를 파싱한 결과다(QU-2EF510B4, 설계자 지시,
    // 2026-09-14 - "IRQ==GSI로 가정하지 말고 실제로 파싱해서 맵핑하라").
    // override가 없는 IRQ는 ISA 기본값(GSI=IRQ 그대로, active-high,
    // edge-triggered)으로 반환한다 - ACPI 스펙상 override 테이블에
    // 없는 ISA IRQ는 이 기본값을 따른다고 명시돼 있다.
    struct IsaIrqRouting {
        uint32_t gsi;
        // kPolarityActiveHigh/ActiveLow(아래 상수) - ACPI MPS INTI
        // Flags의 극성 필드를 ISA 기본값(active-high)까지 반영해
        // 이미 해석해 둔 값이다("conforms" 값은 여기 없음).
        uint32_t polarity;
        // kTriggerEdge/Level(아래 상수) - 위와 같은 이유로 이미 해석됨
        // (ISA 기본값 edge까지 반영).
        uint32_t triggerMode;
    };
    static IsaIrqRouting resolveIsaIrq(uint32_t isaIrq);

    // HPET("HPET" 시그니처)는 선택 테이블이다 - 없는 펌웨어도 흔하다.
    // hasHpet()이 false면 hpetAddress()는 의미 없다(호출부가 반드시
    // hasHpet()로 먼저 확인해야 함, DS-D4E5C451 - "HPET을 스케줄러
    // 기본 시간원으로, LAPIC 타이머는 fallback으로").
    static bool hasHpet();
    static uint64_t hpetAddress();

    // ACPI MCFG("MCFG" 시그니처) - PCIe MMCONFIG(ECAM) 베이스 주소
    // 테이블(QU-7B67E05A/QU-4C2DD71C, 설계자 지시, 2026-09-14 -
    // "MMCONFIG 및 0xCF8 둘 모두 고려하고 준비해야 Legacy fallback을
    // 구현할 수 있다"). 여러 PCI 세그먼트 그룹을 지원하는 시스템도
    // 있지만(서버급), 이 프로젝트는 첫 번째 엔트리(세그먼트 그룹 0)만
    // 다룬다 - 일반 데스크톱/서버 대부분이 세그먼트 그룹 0 하나뿐이라
    // 실용적인 범위(관계도에 기록).
    static bool hasMcfg();
    static uint64_t mcfgBaseAddress();
    static uint8_t mcfgStartBus();
    static uint8_t mcfgEndBus();

    // NUMA 토폴로지 (SRAT 없으면 항상 numaNodeCount()==1로 fallback).
    static uint32_t numaNodeCount();
    // cpuApicId(index)와 같은 index로 대응되는 코어가 속한 노드.
    static uint32_t cpuNumaNode(uint32_t index);

    // 물리 메모리 범위별 소속 노드 - PageFrameAllocator가 usable
    // 메모리를 노드별로 나누는 데 쓴다. SRAT가 없으면 개수 0을
    // 반환한다(호출부는 "정보 없음 = 전부 노드 0"으로 취급해야 함).
    static uint32_t memoryAffinityCount();
    static uint64_t memoryAffinityBase(uint32_t index);
    static uint64_t memoryAffinityLength(uint32_t index);
    static uint32_t memoryAffinityNode(uint32_t index);

    // [신규, 2026-09-23, DC-F367AD5D/QU-7C3AB7A2 답변 - 커널 Shutdown/
    // Reboot 경로] ACPI FADT("FACP" 시그니처) - PM1 이벤트/제어
    // 레지스터 블록(전원 버튼 SCI 처리+실제 절전 진입에 필요)과
    // ACPI 2.0+ Reset Register(RESET_REG_SUP 플래그가 서 있을 때만
    // 유효). `Power` 클래스(power.h)가 이 접근자들을 바탕으로 실제
    // 하드웨어 레지스터를 조작한다 - `Acpi` 자신은 순수 파싱/노출만
    // 담당(다른 모든 접근자와 동일한 역할 분리).
    static bool hasFadt();
    static uint32_t sciInterruptGsi();
    static uint32_t smiCommandPort();
    static uint8_t acpiEnableValue();
    static uint8_t acpiDisableValue();
    static uint32_t pm1aEventBlock();
    static uint32_t pm1bEventBlock();  // 0이면 없음(단일 PM1 블록만 있는 보통의 경우)
    static uint32_t pm1EventBlockLength();
    static uint32_t pm1aControlBlock();
    static uint32_t pm1bControlBlock();  // 0이면 없음
    static uint32_t pm1ControlBlockLength();

    // ACPI 2.0+ Reset Register(FADT flags bit10=RESET_REG_SUP일 때만
    // 유효 - hasResetRegister()로 먼저 확인). addressSpaceId는 ACPI
    // Generic Address Structure 값 그대로(0=시스템 메모리, 1=시스템
    // I/O - 실무에서는 거의 항상 1).
    static bool hasResetRegister();
    static uint8_t resetRegisterAddressSpaceId();
    static uint64_t resetRegisterAddress();
    static uint8_t resetRegisterValue();

    // DSDT(Differentiated System Description Table, "DSDT" 시그니처)
    // AML 바이트코드의 물리 주소/길이 - 이 프로젝트는 범용 AML
    // 인터프리터가 없으므로(그 자체로 별도의 큰 서브시스템) `Power`가
    // \_S5 패키지 하나만 최소로 스캔하는 데 쓴다(power.cpp 참고).
    static uint64_t dsdtPhysAddress();
    static uint32_t dsdtLength();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_ACPI_H
