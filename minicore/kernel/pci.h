#ifndef MINICORE_KERNEL_PCI_H
#define MINICORE_KERNEL_PCI_H

namespace kernel {

// PCI Configuration Space 접근 + 최소 버스 열거 + MSI/MSI-X capability
// 프로그래밍(PL-2070E6EF). 두 접근 메커니즘을 모두 지원한다
// (QU-7B67E05A/QU-4C2DD71C, 설계자 지시, 2026-09-14 - "MMCONFIG 및
// 0xCF8 둘 모두 고려하고 준비해야 Legacy fallback을 구현할 수
// 있다"): ACPI MCFG 테이블이 있으면 bus 0을 MMCONFIG(PCIe 확장
// 설정 공간 4KiB 전부 접근 가능)로 매핑해 쓰고, 그 밖의 버스나
// MCFG가 아예 없는 시스템은 레거시 포트 0xCF8/0xCFC(256바이트
// 제한)로 폴백한다 - **범위 안에서의 실용적 선택**: MCFG는 보통
// 256개 버스 전체(최대 256MiB)를 커버하는데, 이 프로젝트가 실제로
// 열거/사용하는 건 bus 0(과 브리지로 발견되는 하위 버스, 아직
// 실기에서 만난 적 없음)뿐이라 bus 0만 미리 매핑해 둔다 - 다른
// 버스는 자동으로 레거시 경로로 계속 동작한다(관계도에 기록).
class Pci {
public:
    struct Device {
        unsigned char bus, device, function;
        unsigned short vendorId, deviceId;
        unsigned char classCode, subclass, progIf, revisionId;
        unsigned char headerType;  // 멀티펑션 비트(0x80)는 이미 뗀 값
    };

    // Acpi::init() 이후, Paging::init() 이후에 호출해야 한다(MCFG
    // 있으면 bus 0 MMCONFIG 창을 매핑). MCFG가 없으면 아무 것도 안
    // 하고 조용히 레거시 전용으로 남는다.
    static void init();
    // 진단/로그용 - bus 0 접근이 실제로 MMCONFIG를 쓰는지.
    static bool usesMmconfig();

    static unsigned int readConfig32(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset);
    static void writeConfig32(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset, unsigned int value);
    static unsigned short readConfig16(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset);
    static void writeConfig16(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset, unsigned short value);
    static unsigned char readConfig8(unsigned char bus, unsigned char device, unsigned char function, unsigned char offset);

    // bus 0부터 훑어 존재하는 장치(vendorId != 0xFFFF)마다 callback을
    // 부른다 - 멀티펑션 장치(헤더타입 bit7)는 function 1-7도 확인하고,
    // PCI-PCI 브리지(class 0x06/subclass 0x04)는 secondary bus를
    // 재귀적으로 마저 훑는다.
    using EnumerateCallback = void (*)(const Device&);
    static void enumerate(EnumerateCallback callback);

    // Capabilities List(Status 레지스터 bit4 확인 후 0x34의 포인터를
    // 따라감)에서 capabilityId(MSI=0x05, MSI-X=0x11)를 찾는다 - 있으면
    // 설정 공간 오프셋을, 없으면 0을 반환(오프셋 0은 헤더 영역이라
    // capability가 될 수 없어 안전한 sentinel).
    static unsigned char findCapability(unsigned char bus, unsigned char device, unsigned char function, unsigned char capabilityId);

    // 장치의 MSI capability를 벡터 1개로 프로그래밍하고 Enable
    // 비트를 켠다(Multiple Message Enable=0). MSI capability가 없거나
    // destApicId가 클래식 MSI의 8비트 목적지 필드를 초과하면 false.
    // 여러 벡터가 필요하면
    // enableMsiVectors를 쓴다.
    static bool enableMsi(unsigned char bus, unsigned char device, unsigned char function, unsigned int vector, unsigned int destApicId);

    // 장치가 여러 MSI 벡터를 지원하면 그만큼(또는 장치 한도까지)
    // 확보해 부하 분산에 쓸 수 있게 한다(QU-7C65048E, 설계자 지시,
    // 2026-09-14 - "장치가 여러 벡터를 지원하면 부하 분산이 가능하도록
    // 설계하라"). requestedCount는 2의 거듭제곱으로 내림되고 장치의
    // Multiple Message Capable 한도로 다시 제한된다. preferredBase는
    // 원하는 시작 벡터(MSI 하드웨어 제약상 실제로 확보되는 벡터
    // 개수의 배수로 정렬돼야 해서, 필요하면 내부적으로 내림 조정한다) -
    // 실제로 쓰인 시작 벡터/개수를 outBaseVector/outGrantedCount로
    // 돌려준다(호출부가 이 범위의 각 벡터에 Idt::registerHandler를
    // 직접 걸어야 한다 - InterruptFrame::vector로 어느 벡터인지 구분).
    // 실패(capability 없음/destApicId가 클래식 MSI의 8비트 목적지
    // 필드를 초과)하면 false, outGrantedCount=0.
    static bool enableMsiVectors(unsigned char bus, unsigned char device, unsigned char function,
                                  unsigned int requestedCount, unsigned int preferredBase, unsigned int destApicId,
                                  unsigned int* outBaseVector, unsigned int* outGrantedCount);

    // MSI-X capability의 테이블 엔트리 하나(tableIndex)를 벡터/목적지로
    // 프로그래밍한다(QU-A62F3008, 설계자 지시, 2026-09-14 - "실제
    // 테이블 프로그래밍도 지금 시점에서 구현하여 검증하라"). 테이블은
    // BAR(Table Offset/BIR 필드로 지정) 안에 있어 그 BAR를 Paging으로
    // 매핑해야 접근할 수 있다 - 이 함수가 필요한 매핑까지 전부 처리한다.
    // tableIndex가 테이블 크기(Message Control bits0-10)를 넘거나
    // capability가 없으면 false. 성공하면 해당 엔트리의 마스크를
    // 풀고, 함수 전체 마스크(bit14)도 해제하고, MSI-X Enable(bit15)을
    // 켠다.
    static bool enableMsix(unsigned char bus, unsigned char device, unsigned char function, unsigned int tableIndex,
                            unsigned int vector, unsigned int destApicId);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PCI_H
