// UEFI 최소 타입 서브셋 - PN-7FBF255A 체크리스트 2번. 지금은
// efi_main의 시그니처를 표현하는 데 필요한 만큼만 정의한다(EFI_STATUS/
// EFI_HANDLE/EFI_SYSTEM_TABLE) - EFI_SYSTEM_TABLE은 아직 필드를
// 하나도 역참조하지 않으므로(체크리스트 4번의 GetMemoryMap/GOP 조회
// 단계에서 실제 필드가 채워진다) 지금은 불완전 타입으로만 전방
// 선언해 둔다(RM-23F4B687 §4 - 아직 안 쓰는 필드를 미리 채워 넣지
// 않는다).
#ifndef MINICORE_BOOT_UEFI_EFI_TYPES_H
#define MINICORE_BOOT_UEFI_EFI_TYPES_H

using EFI_STATUS = unsigned long long;
using EFI_HANDLE = void*;

struct EFI_SYSTEM_TABLE;

constexpr EFI_STATUS kEfiSuccess = 0;

#endif  // MINICORE_BOOT_UEFI_EFI_TYPES_H
