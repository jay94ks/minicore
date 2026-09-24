// UEFI 스테이지 로더가 kMain에 넘기는 부팅 정보 - SP-CC2B18C6/
// DC-F196028B(설계자 답변: "부팅 정보 구조체를 통합해") - boot.S
// 마커에 스크래치 필드를 하나씩 더 추가하는 대신, physicalBaseDelta/
// 메모리맵/ACPI RSDP를 이 구조체 하나로 묶어 그 물리주소만
// saved_start_info(기존 채널, boot.S 변경 불필요)로 넘긴다. 이
// 구조체 자신은 재배치된 커널 이미지가 아니라 UEFI 로더 자신의
// 정적 메모리(낮은 1GiB 안, pd_low identity map으로 CR3 전환
// 이후에도 그대로 역참조 가능 - main.cpp의 기존 belowOneGiB 전제와
// 동일)에 산다.
//
// **커널 쪽에 이 구조체의 독립된 사본이 있다**
// (minicore/kernel/uefi_boot_info.h) - 두 빌드 트리가 완전히
// 분리돼 있어(efi/elf.h 등과 같은 이유) 헤더를 공유할 수 없다.
// **두 사본은 바이트 단위로 반드시 동일해야 한다** - 필드를
// 추가/변경하면 양쪽을 함께 고친다.
#ifndef MINICORE_BOOT_UEFI_EFI_BOOT_INFO_H
#define MINICORE_BOOT_UEFI_EFI_BOOT_INFO_H

struct UefiBootInfo {
    // SP-CC2B18C6 §2 - actualPhysicalLoadBase - KERNEL_LMA.
    unsigned long long physicalBaseDelta;
    // GetMemoryMap()이 마지막으로 채운 원시 EFI_MEMORY_DESCRIPTOR
    // 배열의 물리주소(=gMemoryMapBuffer 자신의 주소, UEFI 로더가
    // ExitBootServices 이후에도 그 메모리를 계속 보존해 둔다) -
    // 커널이 이 배열을 직접 순회해 HvmMemmapEntry로 변환한다
    // (Multiboot2Info::parse와 같은 패턴, UEFI 명세 그대로
    // sizeof()가 아니라 memmapDescriptorSize를 보폭으로 써야 함).
    unsigned long long memmapPaddr;
    unsigned long long memmapDescriptorSize;
    unsigned int memmapEntryCount;
    // ConfigurationTable에서 찾은 ACPI RSDP 물리주소 - 못 찾으면 0
    // (커널이 기존 GRUB/PVH 경로처럼 "ACPI MADT parse FAILED"로
    // 정직하게 로그).
    unsigned long long rsdpPaddr;
};

#endif  // MINICORE_BOOT_UEFI_EFI_BOOT_INFO_H
