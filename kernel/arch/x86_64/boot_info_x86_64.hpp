// x86_64 boot_info 파이프라인 (docs/spec/boot.md §1.1/§2~3,
// docs/plan/kernel-bootstrap.md M2). Multiboot2 태그를 파싱해 arch
// 독립 boot::boot_info(kernel/include/boot_info.hpp)로 변환한다.
#pragma once

#include "acpi.hpp"
#include "boot_info.hpp"

#include <cstdint>

namespace kern::arch::x86_64 {

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

// M11(smp-fpu-bringup.md §M11, ADR-036) — SRAT 메모리 어피니티가 실제로
// 존재할 때(QEMU `-numa`로 노드별 memory-backend-ram이 구성됐을 때)
// 그 실제 물리 범위를 usable 리전으로 삼아 boot_info를 구성한다 —
// self-test fixture(단일 노드 0 가짜 데이터)와는 완전히 별개의 경로.
// `srat.mem_affinity_count == 0`이면 호출하지 않고
// `run_boot_info_self_test()`를 그대로 써야 한다(기존 M1~M10 경로
// 보존, NUMA 미구성 시 관찰 가능한 차이 없음). *out_cpu_node_map에는
// `madt.cpu_count`개짜리 uint32_t 배열의 역참조 가능한 포인터를
// 채운다(다음 호출까지만 유효).
boot::boot_info build_numa_boot_info(const madt_result& madt, const srat_slit_result& srat,
                                      const boot::memory_region** out_regions,
                                      const uint32_t** out_cpu_node_map);

}  // namespace kern::arch::x86_64
