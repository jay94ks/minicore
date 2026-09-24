// EFI_MEMORY_DESCRIPTOR - PN-7FBF255A 체크리스트 4번 나머지(실제
// 메모리맵 데이터 파싱). UEFI 명세 필드 순서 그대로 - Type(4바이트)
// 다음 8바이트 정렬 필드가 오므로 컴파일러가 자동으로 4바이트
// 패딩을 넣는다(system_table.h와 동일한 자연 정렬 원칙).
//
// **주의(UEFI 명세 그대로)**: 이 구조체를 `sizeof()`로 배열 인덱싱에
// 쓰면 안 된다 - 펌웨어가 GetMemoryMap()으로 돌려주는 실제
// `DescriptorSize`가 이 구조체보다 클 수 있다(향후 명세 확장 대비,
// 항목 사이에 여분 바이트가 있을 수 있음) - 반드시 그 값을 보폭으로
// 써서 순회해야 한다(main.cpp에서 실제로 이렇게 함).
#ifndef MINICORE_BOOT_UEFI_EFI_MEMORY_H
#define MINICORE_BOOT_UEFI_EFI_MEMORY_H

struct EFI_MEMORY_DESCRIPTOR {
    unsigned int Type;
    unsigned long long PhysicalStart;
    unsigned long long VirtualStart;
    unsigned long long NumberOfPages;
    unsigned long long Attribute;
};

// UEFI 명세 EFI_MEMORY_TYPE 열거값 중 지금 실제로 구분하는 것 하나만
// (일반 사용 가능 RAM) - 나머지 13개 값은 아직 안 쓴다.
constexpr unsigned int kEfiConventionalMemory = 7;

#endif  // MINICORE_BOOT_UEFI_EFI_MEMORY_H
