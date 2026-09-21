// EFI_SYSTEM_TABLE/EFI_BOOT_SERVICES/EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
// 최소 서브셋 - PN-7FBF255A 체크리스트 4번(GetMemoryMap 조회 +
// ConOut으로 상태 출력).
//
// **의도적으로 UEFI 명세 전체를 옮기지 않는다(RM-23F4B687 §4)** -
// 각 구조체는 "실제로 지금 쓰는 필드까지"만 선언한다. 이게 안전한
// 이유: 이 구조체들은 전부 펌웨어가 이미 채워 둔 메모리를 가리키는
// **포인터**로만 받아 그 필드를 읽을 뿐, 이 타입 자체로 인스턴스를
// 만들거나 sizeof()로 크기를 계산해 통째로 복사하지 않는다 - 그래서
// "구조체 뒷부분을 선언 안 함"은 안전하고, "앞부분 필드 순서/타입
// (=오프셋)가 실제 UEFI 명세와 정확히 일치"하는 것만 중요하다.
// 뒤에 필요한 필드가 생기면(체크리스트 5번 - ExitBootServices 등)
// 그때 이어서 선언을 늘린다.
#ifndef MINICORE_BOOT_UEFI_EFI_SYSTEM_TABLE_H
#define MINICORE_BOOT_UEFI_EFI_SYSTEM_TABLE_H

#include "types.h"

struct EFI_TABLE_HEADER {
    unsigned long long Signature;
    unsigned int Revision;
    unsigned int HeaderSize;
    unsigned int CRC32;
    unsigned int Reserved;
};

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void* Reset;  // EFI_TEXT_RESET - 안 씀, 자리만 차지(오프셋 유지용)
    EFI_STATUS(EFIAPI* OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, CHAR16* String);
    // 이 프로토콜의 나머지 필드(TestString/QueryMode/... /Mode)는
    // 아직 안 씀 - 선언하지 않는다(위 문서 주석 참고).
};

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void* RaiseTPL;    // 안 씀, 자리만 차지
    void* RestoreTPL;  // 안 씀, 자리만 차지
    void* AllocatePages;  // 안 씀, 자리만 차지
    void* FreePages;      // 안 씀, 자리만 차지
    EFI_STATUS(EFIAPI* GetMemoryMap)(unsigned long long* MemoryMapSize, void* MemoryMap,
                                      unsigned long long* MapKey, unsigned long long* DescriptorSize,
                                      unsigned int* DescriptorVersion);
    // [신규, PN-7FBF255A 체크리스트 5번 - 커널 ELF 로더] AllocatePool
    // ~ UninstallProtocolInterface까지는 안 씀 - UEFI 명세의 실제
    // 필드 순서 그대로 자리만 차지(오프셋 유지, 이 파일 상단 문서
    // 주석의 관례 그대로).
    void* AllocatePool;
    void* FreePool;
    void* CreateEvent;
    void* SetTimer;
    void* WaitForEvent;
    void* SignalEvent;
    void* CloseEvent;
    void* CheckEvent;
    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    // efi_main이 LoadedImageProtocol(imageHandle)/SimpleFileSystemProtocol
    // (DeviceHandle)을 얻는 데 실제로 쓴다 - 둘 다 특정 핸들이 이미
    // 손에 있는 경우라 LocateProtocol이 아니라 이 함수로 충분하다.
    EFI_STATUS(EFIAPI* HandleProtocol)(EFI_HANDLE Handle, EFI_GUID* Protocol, void** Interface);
    // Reserved 이후(RegisterProtocolNotify ~ LocateHandleBuffer, 그
    // 사이의 LoadImage/StartImage/Exit/UnloadImage/ExitBootServices/
    // GetNextMonotonicCount/Stall/SetWatchdogTimer/ConnectController/
    // DisconnectController/OpenProtocol/CloseProtocol/
    // OpenProtocolInformation/ProtocolsPerHandle 포함)는 아직 안 씀 -
    // 선언하지 않는다(LocateProtocol/ExitBootServices/AllocatePages의
    // 실제 시그니처는 그 함수들을 실제로 쓰는 다음 증분에서 추가).
};

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16* FirmwareVendor;
    unsigned int FirmwareRevision;
    // FirmwareRevision(4바이트) 다음은 8바이트 정렬 포인터 필드라
    // 컴파일러가 자동으로 4바이트 패딩을 넣는다(#pragma pack 없음 -
    // 실제 UEFI 명세 구조체도 자연 정렬에 의존한다).
    EFI_HANDLE ConsoleInHandle;
    void* ConIn;  // EFI_SIMPLE_TEXT_INPUT_PROTOCOL* - 안 씀, 자리만 차지
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    EFI_HANDLE StandardErrorHandle;
    void* StdErr;            // 안 씀, 자리만 차지
    void* RuntimeServices;   // 안 씀, 자리만 차지
    EFI_BOOT_SERVICES* BootServices;
    // NumberOfTableEntries/ConfigurationTable 이후는 아직 안 씀 -
    // 선언하지 않는다.
};

#endif  // MINICORE_BOOT_UEFI_EFI_SYSTEM_TABLE_H
