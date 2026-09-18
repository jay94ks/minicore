// UEFI 최소 타입 서브셋 - PN-7FBF255A 체크리스트 2/4번. 지금
// efi_main이 실제로 쓰는 만큼만 정의한다(RM-23F4B687 §4 - 아직 안
// 쓰는 필드/코드는 미리 채워 넣지 않는다) - EFI_SYSTEM_TABLE 자체의
// 필드 레이아웃은 efi/system_table.h(체크리스트 4번)가 담당한다.
#ifndef MINICORE_BOOT_UEFI_EFI_TYPES_H
#define MINICORE_BOOT_UEFI_EFI_TYPES_H

using EFI_STATUS = unsigned long long;
using EFI_HANDLE = void*;
using CHAR16 = char16_t;
using BOOLEAN = unsigned char;

// UEFI 명세의 호출 규약 표시자 - x86_64-unknown-windows 타깃에서는
// 이미 기본 호출 규약 자체가 MS x64(EFIAPI가 요구하는 그것)라 별도
// 속성이 필요 없다(efi_main 자신도 그냥 extern "C"로 이미 맞음,
// PN-7FBF255A "실행 가능성 조사" 절 참고) - 명세 문서와 나란히 읽기
// 쉽도록 이름만 남겨 둔다.
#define EFIAPI

struct EFI_SYSTEM_TABLE;

// UEFI 명세 - x64에서 오류 상태는 최상위 비트(0x8000000000000000)가
// 서 있다. 지금 실제로 구분해야 하는 값 둘만 정의한다(체크리스트
// 4번 GetMemoryMap이 크기 질의 시 항상 EFI_BUFFER_TOO_SMALL을
// 반환하는 것을 확인하는 데 필요).
constexpr EFI_STATUS kEfiErrorBit = 0x8000000000000000ULL;
constexpr EFI_STATUS kEfiSuccess = 0;
constexpr EFI_STATUS kEfiBufferTooSmall = kEfiErrorBit | 5;

#endif  // MINICORE_BOOT_UEFI_EFI_TYPES_H
