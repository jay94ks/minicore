// Minicore UEFI 직접 부팅 진입점 - PN-7FBF255A 체크리스트 2/3번.
// 지금은 아무 것도 안 하고 EFI_SUCCESS로 즉시 반환하는 최소
// 스텁이다(이전 틱의 scratchpad PoC를 그대로 정식 저장소 코드로
// 옮긴 것 - QEMU+OVMF 부팅 검증 결과는 PN-7FBF255A "체크리스트 1번
// 실측 검증" 절 참고). 다음 증분(체크리스트 4/5번)이 EFI_SYSTEM_TABLE
// 에서 메모리맵/GOP를 읽고 ExitBootServices() 이후 자체 GDT/페이지
// 테이블로 전환해 기존 higher_half_entry/kMain에 합류한다.
#include "efi/types.h"

extern "C" EFI_STATUS efi_main(EFI_HANDLE /*imageHandle*/, EFI_SYSTEM_TABLE* /*systemTable*/) {
    return kEfiSuccess;
}
