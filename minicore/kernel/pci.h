#ifndef MINICORE_KERNEL_PCI_H
#define MINICORE_KERNEL_PCI_H

namespace kernel {

// PCI Configuration Space 접근(레거시 메커니즘 #1 - 포트 0xCF8/0xCFC)
// + 최소 버스 열거 + MSI capability 프로그래밍(PL-2070E6EF 0단계 -
// MSI/MSI-X 지원의 선행 조건, 이 프로젝트엔 PCI 코드 자체가 이전엔
// 전혀 없었다). v1은 MMCONFIG(ACPI MCFG 기반, PCIe 확장 설정 공간
// 4KiB)를 다루지 않고 레거시 256바이트 설정 공간만 접근한다 - MSI
// capability는 이 범위 안에 있어 지장 없다(PL 자체가 이미 이렇게
// 범위를 정함 - PCIe 확장 기능이 실제로 필요해지면 그때 MMCONFIG를
// 추가한다).
class Pci {
public:
    struct Device {
        unsigned char bus, device, function;
        unsigned short vendorId, deviceId;
        unsigned char classCode, subclass, progIf, revisionId;
        unsigned char headerType;  // 멀티펑션 비트(0x80)는 이미 뗀 값
    };

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

    // 장치의 MSI capability를 벡터/목적지 APIC ID로 프로그래밍하고
    // Enable 비트를 켠다(Multiple Message Enable은 항상 0=벡터 1개로
    // 고정 - 여러 벡터를 요청하는 장치 지원은 범위 밖). MSI capability가
    // 없으면 false. **MSI-X는 범위 밖**(BAR 안의 별도 테이블에 써야
    // 해서 구조가 다르다 - PL 완료 기록의 "안 한 것" 참고).
    static bool enableMsi(unsigned char bus, unsigned char device, unsigned char function, unsigned int vector, unsigned int destApicId);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PCI_H
