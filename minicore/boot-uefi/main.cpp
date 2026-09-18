// Minicore UEFI 직접 부팅 진입점 - PN-7FBF255A 체크리스트 2/3/4번.
// 체크리스트 4번: EFI_SYSTEM_TABLE에서 ConOut으로 진행 상태를 찍고,
// BootServices->GetMemoryMap을 크기 질의 모드(MemoryMap=nullptr)로
// 호출해 EFI_BUFFER_TOO_SMALL과 함께 돌아오는 필요 크기를 확인한다 -
// 아직 실제 메모리맵 배열을 받아 파싱하지는 않는다(그 단계는 이
// 구조체 서브셋이 GOP/메모리맵 항목 구조체까지 늘어나야 하는 다음
// 증분 몫, RM-23F4B687 §4 - 지금 안 쓰는 걸 미리 만들지 않는다).
// 다음 증분(체크리스트 4번 나머지 + 5번)이 실제 메모리맵을 받아
// ExitBootServices() 이후 자체 GDT/페이지 테이블로 전환해 기존
// higher_half_entry/kMain에 합류한다.
#include "efi/system_table.h"
#include "efi/types.h"

namespace {

// UEFI에는 표준 라이브러리가 없어 64비트 값을 CHAR16 10진 문자열로
// 직접 변환한다 - GetMemoryMap이 돌려준 필요 크기를 사람이 읽을 수
// 있게 ConOut에 찍기 위한 용도뿐이라 이 파일 안에만 필요한 만큼
// 최소로 둔다.
void kFormatUint64(unsigned long long value, CHAR16* out, unsigned int outCapacity) {
    CHAR16 digits[20];
    unsigned int digitCount = 0;
    if (value == 0) {
        digits[digitCount++] = u'0';
    }
    while (value > 0 && digitCount < 20) {
        digits[digitCount++] = static_cast<CHAR16>(u'0' + (value % 10));
        value /= 10;
    }
    unsigned int i = 0;
    for (; i < digitCount && i + 1 < outCapacity; ++i) {
        out[i] = digits[digitCount - 1 - i];
    }
    out[i] = u'\0';
}

}  // namespace

extern "C" EFI_STATUS efi_main(EFI_HANDLE /*imageHandle*/, EFI_SYSTEM_TABLE* systemTable) {
    if (!systemTable) {
        return kEfiSuccess;
    }

    if (systemTable->ConOut) {
        systemTable->ConOut->OutputString(systemTable->ConOut, const_cast<CHAR16*>(u"minicore: efi_main reached\r\n"));
    }

    if (systemTable->BootServices) {
        // 크기 질의 모드(UEFI 명세 그대로) - MemoryMapSize를 실제보다
        // 작게(0으로) 넘기면 항상 EFI_BUFFER_TOO_SMALL을 반환하면서
        // MemoryMapSize를 "실제 필요한 크기"로 덮어써 돌려준다.
        unsigned long long memoryMapSize = 0;
        unsigned long long mapKey = 0;
        unsigned long long descriptorSize = 0;
        unsigned int descriptorVersion = 0;
        EFI_STATUS status = systemTable->BootServices->GetMemoryMap(&memoryMapSize, nullptr, &mapKey,
                                                                      &descriptorSize, &descriptorVersion);
        if (systemTable->ConOut) {
            if (status == kEfiBufferTooSmall) {
                CHAR16 sizeStr[24];
                kFormatUint64(memoryMapSize, sizeStr, 24);
                systemTable->ConOut->OutputString(systemTable->ConOut,
                                                   const_cast<CHAR16*>(u"minicore: GetMemoryMap size query ok, required="));
                systemTable->ConOut->OutputString(systemTable->ConOut, sizeStr);
                systemTable->ConOut->OutputString(systemTable->ConOut, const_cast<CHAR16*>(u"\r\n"));
            } else {
                systemTable->ConOut->OutputString(
                    systemTable->ConOut, const_cast<CHAR16*>(u"minicore: GetMemoryMap size query unexpected status\r\n"));
            }
        }
    }

    return kEfiSuccess;
}
