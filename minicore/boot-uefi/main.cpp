// Minicore UEFI 직접 부팅 진입점 - PN-7FBF255A 체크리스트 2/3/4번.
// 체크리스트 4번: EFI_SYSTEM_TABLE에서 ConOut으로 진행 상태를 찍고,
// BootServices->GetMemoryMap을 실제로 두 번 호출한다 - 먼저 크기
// 질의(EFI_BUFFER_TOO_SMALL), 그다음 정적 버퍼(gMemoryMapBuffer)로
// 데이터 모드 호출 - 받은 메모리맵을 DescriptorSize 보폭으로 순회해
// EfiConventionalMemory(일반 사용 가능 RAM) 페이지 수를 합산한다.
// AllocatePool 대신 정적 버퍼를 쓴 이유: 이 질의 하나만을 위해
// EFI_BOOT_SERVICES 서브셋을 AllocatePool까지 더 늘리지 않기 위함
// (RM-23F4B687 §4, PN-7FBF255A 체크리스트 4번이 "AllocatePool 또는
// 정적 버퍼"로 이미 열어 둔 선택지).
//
// GOP(그래픽 출력 프로토콜) 조회와 ExitBootServices() 핸드오프(체크
// 리스트 4번 나머지 + 5번)는 여전히 다음 증분 몫이다.
#include "efi/memory.h"
#include "efi/system_table.h"
#include "efi/types.h"

namespace {

// UEFI에는 표준 라이브러리가 없어 64비트 값을 CHAR16 10진 문자열로
// 직접 변환한다 - GetMemoryMap이 돌려준 크기/페이지 수를 사람이 읽을
// 수 있게 ConOut에 찍기 위한 용도뿐이라 이 파일 안에만 필요한 만큼
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

void kPrint(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* conOut, const CHAR16* text) {
    conOut->OutputString(conOut, const_cast<CHAR16*>(text));
}

void kPrintUint64(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* conOut, unsigned long long value) {
    CHAR16 buf[24];
    kFormatUint64(value, buf, 24);
    conOut->OutputString(conOut, buf);
}

// 실측 확인(PN-7FBF255A 체크리스트 4번 - 크기 질의 모드 검증)한 필요
// 크기(6528/6336바이트, 부팅마다 약간 다름)보다 5배 가까이 넉넉한
// 정적 버퍼 - AllocatePool 없이도 항상 한 번에 담긴다.
constexpr unsigned long long kMemoryMapBufferCapacity = 32 * 1024;
unsigned char gMemoryMapBuffer[kMemoryMapBufferCapacity];

}  // namespace

extern "C" EFI_STATUS efi_main(EFI_HANDLE /*imageHandle*/, EFI_SYSTEM_TABLE* systemTable) {
    if (!systemTable) {
        return kEfiSuccess;
    }

    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* conOut = systemTable->ConOut;  // 아래에서 반복 참조 - nullptr일 수 있음, 매번 확인
    if (conOut) {
        kPrint(conOut, u"minicore: efi_main reached\r\n");
    }

    if (systemTable->BootServices) {
        // 1) 크기 질의 모드(UEFI 명세 그대로) - MemoryMapSize를
        // 실제보다 작게(0으로) 넘기면 항상 EFI_BUFFER_TOO_SMALL을
        // 반환하면서 MemoryMapSize를 "실제 필요한 크기"로 덮어써
        // 돌려준다 - 정적 버퍼가 실제로 충분한지 사전 확인하는
        // 용도로도 재사용한다(버퍼가 작으면 여기서 미리 알 수 있음).
        unsigned long long queriedSize = 0;
        unsigned long long mapKey = 0;
        unsigned long long descriptorSize = 0;
        unsigned int descriptorVersion = 0;
        EFI_STATUS queryStatus =
            systemTable->BootServices->GetMemoryMap(&queriedSize, nullptr, &mapKey, &descriptorSize, &descriptorVersion);

        if (conOut) {
            if (queryStatus == kEfiBufferTooSmall) {
                kPrint(conOut, u"minicore: GetMemoryMap size query ok, required=");
                kPrintUint64(conOut, queriedSize);
                kPrint(conOut, u"\r\n");
            } else {
                kPrint(conOut, u"minicore: GetMemoryMap size query unexpected status\r\n");
            }
        }

        // 2) 데이터 모드 - 정적 버퍼(kMemoryMapBufferCapacity)가 위
        // 질의 결과보다 충분히 크다고 확신하지 않고 항상 실측 확인한다.
        if (queryStatus == kEfiBufferTooSmall && queriedSize <= kMemoryMapBufferCapacity) {
            unsigned long long mapSize = kMemoryMapBufferCapacity;
            EFI_STATUS dataStatus = systemTable->BootServices->GetMemoryMap(&mapSize, gMemoryMapBuffer, &mapKey,
                                                                              &descriptorSize, &descriptorVersion);
            if (dataStatus == kEfiSuccess && descriptorSize > 0) {
                // [중요, UEFI 명세] sizeof(EFI_MEMORY_DESCRIPTOR)가
                // 아니라 펌웨어가 돌려준 descriptorSize를 보폭으로
                // 순회해야 한다(efi/memory.h 문서 주석 참고).
                const unsigned long long entryCount = mapSize / descriptorSize;
                unsigned long long conventionalPages = 0;
                for (unsigned long long i = 0; i < entryCount; ++i) {
                    const auto* desc =
                        reinterpret_cast<const EFI_MEMORY_DESCRIPTOR*>(gMemoryMapBuffer + i * descriptorSize);
                    if (desc->Type == kEfiConventionalMemory) {
                        conventionalPages += desc->NumberOfPages;
                    }
                }
                if (conOut) {
                    kPrint(conOut, u"minicore: memory map entries=");
                    kPrintUint64(conOut, entryCount);
                    kPrint(conOut, u" conventionalPages=");
                    kPrintUint64(conOut, conventionalPages);
                    kPrint(conOut, u"\r\n");
                }
            } else if (conOut) {
                kPrint(conOut, u"minicore: GetMemoryMap data-mode call unexpected status\r\n");
            }
        } else if (conOut && queryStatus == kEfiBufferTooSmall) {
            kPrint(conOut, u"minicore: static memory map buffer too small - increase kMemoryMapBufferCapacity\r\n");
        }
    }

    return kEfiSuccess;
}
