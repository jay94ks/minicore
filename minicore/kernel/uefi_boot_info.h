#ifndef MINICORE_KERNEL_UEFI_BOOT_INFO_H
#define MINICORE_KERNEL_UEFI_BOOT_INFO_H

#include "libkenv/types.h"

namespace kernel {

// UEFI 스테이지 로더(minicore/boot/x86_64/uefi/main.cpp)가 kMain에
// 넘기는 부팅 정보 - SP-CC2B18C6/DC-F196028B(설계자 답변: "부팅
// 정보 구조체를 통합해"). `startInfoAddr`(kBootProtocolUefi 경로)가
// 이 구조체의 물리주소다.
//
// **UEFI 쪽에 이 구조체의 독립된 사본이 있다**
// (minicore/boot/x86_64/uefi/efi/boot_info.h) - 두 빌드 트리가
// 완전히 분리돼 있어(efi/elf.h 등과 같은 이유로) 헤더를 공유할 수
// 없다. **두 사본은 바이트 단위로 반드시 동일해야 한다.**
struct UefiBootInfo {
    uint64_t physicalBaseDelta;
    uint64_t memmapPaddr;
    uint64_t memmapDescriptorSize;
    uint32_t memmapEntryCount;
    uint64_t rsdpPaddr;
};

// UEFI 명세 EFI_MEMORY_DESCRIPTOR(efi/memory.h의 UEFI 쪽 사본과
// 동일한 이유로 커널 쪽에도 최소 서브셋을 둔다) - GetMemoryMap()이
// 돌려주는 배열 원소 하나. **주의 1**: 배열을 순회할 때 이
// 구조체의 sizeof()가 아니라 `UefiBootInfo::memmapDescriptorSize`를
// 보폭으로 써야 한다(UEFI 명세 - 펌웨어가 이 구조체보다 큰
// DescriptorSize를 돌려줄 수 있음, efi/memory.h 원본 주석과 동일).
// **주의 2, 실측으로 발견한 버그**: 이 구조체는 `packed`이면 안
// 된다 - `type`(4바이트) 다음 8바이트 정렬 필드가 오는 자연 정렬
// 레이아웃이 실제 UEFI 명세이고, efi/memory.h(UEFI 쪽 사본)도
// `packed` 없이 그 자연 정렬(컴파일러가 4바이트 패딩 자동 삽입)에
// 의존한다 - 여기서 `packed`를 붙이면 그 패딩이 사라져
// `physicalStart` 이후 모든 필드가 4바이트씩 밀려 읽혀 완전히
// 엉뚱한(비정상적으로 거대한) 값이 나온다(UEFI 스테이지 로더
// 실측 중 발견 - kLogMemoryMap 출력이 base=0x685c... 같은 말도 안
// 되는 값을 찍어 확인).
struct EfiMemoryDescriptor {
    uint32_t type;
    uint64_t physicalStart;
    uint64_t virtualStart;
    uint64_t numberOfPages;
    uint64_t attribute;
};

// UEFI 명세 EFI_MEMORY_TYPE 중 ExitBootServices 이후 OS가 실제로
// 재사용 가능한 값들 - kUefiMapMemType()이 이 값들을 HvmMemmapType
// (E820 스타일)으로 변환하는 데 쓴다.
enum class EfiMemoryType : uint32_t {
    kReservedMemoryType = 0,
    kLoaderCode = 1,
    kLoaderData = 2,
    kBootServicesCode = 3,
    kBootServicesData = 4,
    kRuntimeServicesCode = 5,
    kRuntimeServicesData = 6,
    kConventionalMemory = 7,
    kUnusableMemory = 8,
    kAcpiReclaimMemory = 9,
    kAcpiMemoryNvs = 10,
    kMemoryMappedIo = 11,
    kMemoryMappedIoPortSpace = 12,
    kPalCode = 13,
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_UEFI_BOOT_INFO_H
