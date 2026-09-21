// [신규, PN-7FBF255A 체크리스트 5번 - 커널 ELF 로더] efi_main이
// minicore.elf를 ESP에서 직접 읽어 들이는 데 필요한 최소 프로토콜
// 서브셋 - EFI_LOADED_IMAGE_PROTOCOL(우리 자신을 실행한 볼륨의
// DeviceHandle을 얻음) + EFI_SIMPLE_FILE_SYSTEM_PROTOCOL/
// EFI_FILE_PROTOCOL(그 볼륨에서 파일을 열고 읽음).
//
// system_table.h와 동일한 원칙(RM-23F4B687 §4) - 각 구조체는 지금
// 실제로 쓰는 필드까지만, 순서/타입은 UEFI 명세와 정확히 일치.
#ifndef MINICORE_BOOT_UEFI_EFI_FILE_H
#define MINICORE_BOOT_UEFI_EFI_FILE_H

#include "system_table.h"
#include "types.h"

// UEFI 명세 §10.3 EFI_LOADED_IMAGE_PROTOCOL GUID.
constexpr EFI_GUID kEfiLoadedImageProtocolGuid = {
    0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

// UEFI 명세 §13.4 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL GUID.
constexpr EFI_GUID kEfiSimpleFileSystemProtocolGuid = {
    0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

struct EFI_FILE_PROTOCOL;

// [UEFI 명세 §10.3] Revision/ParentHandle/SystemTable/DeviceHandle
// 순서 그대로 - DeviceHandle이 이 목적의 유일한 실사용 필드다(우리
// 자신(.efi)이 어느 블록 장치/파티션에서 로드됐는지 식별 - 커널
// ELF도 같은 ESP에 있으므로 이 핸들로 SimpleFileSystemProtocol을
// 얻으면 된다). FilePath 이후는 아직 안 씀.
struct EFI_LOADED_IMAGE_PROTOCOL {
    unsigned int Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE* SystemTable;
    EFI_HANDLE DeviceHandle;
    // FilePath 이후는 아직 안 씀 - 선언하지 않는다.
};

// [UEFI 명세 §13.4] OpenVolume 하나만 쓴다 - 루트 디렉터리
// EFI_FILE_PROTOCOL을 얻는 유일한 진입점.
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    unsigned long long Revision;
    EFI_STATUS(EFIAPI* OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* This, EFI_FILE_PROTOCOL** Root);
};

// 읽기 전용으로 기존 파일을 여는 데 필요한 최소 플래그(UEFI 명세
// §13.5) - CREATE/WRITE는 안 씀(커널 ELF는 이미 ESP에 있는 파일을
// 읽기만 하면 됨).
constexpr unsigned long long kEfiFileModeRead = 0x1ULL;

// [UEFI 명세 §13.5] Revision부터 Flush까지 명세 순서 그대로(Revision
// 1 기준 - OpenEx/ReadEx/WriteEx/FlushEx는 Revision 2 확장이라 안
// 씀). Read()는 호출 시 *BufferSize에 요청 크기를 넣고, 반환 시
// 실제로 읽힌 바이트 수로 덮어쓴다(파일 끝에 도달하면 요청보다 적게
// 채워질 수 있음 - GetMemoryMap의 in/out 관례와 동일).
struct EFI_FILE_PROTOCOL {
    unsigned long long Revision;
    EFI_STATUS(EFIAPI* Open)(EFI_FILE_PROTOCOL* This, EFI_FILE_PROTOCOL** NewHandle, CHAR16* FileName,
                              unsigned long long OpenMode, unsigned long long Attributes);
    EFI_STATUS(EFIAPI* Close)(EFI_FILE_PROTOCOL* This);
    void* Delete;  // 안 씀, 자리만 차지
    EFI_STATUS(EFIAPI* Read)(EFI_FILE_PROTOCOL* This, unsigned long long* BufferSize, void* Buffer);
    void* Write;        // 안 씀, 자리만 차지
    void* GetPosition;  // 안 씀, 자리만 차지
    void* SetPosition;  // 안 씀, 자리만 차지
    void* GetInfo;      // 안 씀, 자리만 차지(파일 크기는 Read() 반환값으로 대신 확인)
    void* SetInfo;      // 안 씀, 자리만 차지
    void* Flush;        // 안 씀, 자리만 차지
};

#endif  // MINICORE_BOOT_UEFI_EFI_FILE_H
