// x86_64 boot_info 파이프라인 (docs/spec/boot.md §1.1/§2~3,
// docs/plan/kernel-bootstrap.md M2). Multiboot2 태그를 파싱해 arch
// 독립 boot::boot_info(kernel/include/boot_info.hpp)로 변환한다.
#pragma once

#include "boot_info.hpp"

#include <cstdint>

namespace arch_x86_64 {

// Multiboot2 부트로더가 EAX에 넣는 매직 값(boot.md §1.1). PVH 등 다른
// 경로로 진입했다면(ADR-114) 이 값과 일치하지 않는다.
inline constexpr uint32_t k_multiboot2_bootloader_magic = 0x36D76289u;

// mb2_magic/mb2_info_addr(boot.S의 _start32가 저장한 EAX/EBX 원본값)로
// boot_info를 구성한다. multiboot_magic이 k_multiboot2_bootloader_magic과
// 다르면(Multiboot2로 부팅되지 않음, 예: QEMU PVH 개발 경로) 파싱을
// 생략하고 최소 필드만 채운 빈 boot_info를 반환한다.
//
// *out_regions에는 info.memory_map_count개짜리 boot::memory_region
// 배열의 (물리주소가 아니라) 그대로 역참조 가능한 포인터를 채운다 —
// 커널 이미지 내부 정적 저장소를 가리키며, 다음 build_boot_info() 호출
// 전까지만 유효하다(재호출 시 덮어써진다).
boot::boot_info build_boot_info(uint32_t multiboot_magic, uint32_t multiboot_info_phys,
                                 const boot::memory_region** out_regions);

// QEMU 개발 경로(ADR-114, PVH)에는 GRUB가 없어 실제 Multiboot2 정보가
// 들어오지 않는다 — build_boot_info()가 태그를 실제로 올바르게 파싱하는지
// QEMU에서 반복 가능하게 확인하려고, 손으로 만든 최소 Multiboot2 info
// 블록을 커널 이미지 안에 두고 그것을 build_boot_info()로 파싱한다.
// 실제 GRUB가 준 정보가 아니므로 호출자는 결과를 klog에 "selftest"
// 태그로 구분해 출력해야 한다.
boot::boot_info run_boot_info_self_test(const boot::memory_region** out_regions);

}  // namespace arch_x86_64
