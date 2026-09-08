// ACPI RSDP 검색 + MADT(Multiple APIC Description Table) 파싱 (docs/plan/
// smp-fpu-bringup.md §M10, ADR-006 — 서드파티 ACPI 라이브러리 금지,
// 필요한 최소 파싱만 직접 구현).
//
// boot.md §2는 "arch_data(ACPI RSDP)는 Multiboot2 태그 14/15로 얻는다"고
// 정했지만, 이 개발 머신은 QEMU PVH 직접 부팅(ADR-114)이라 그 태그가
// 절대 오지 않는다(boot_info.arch_data_addr가 항상 0). 그러나 QEMU의
// ACPI 테이블 자체는 부트 경로와 무관하게 machine 모델(q35)이 항상
// 구성해 guest RAM에 둔다 — ACPI 스펙이 원래 정의하는 "부트로더 도움
// 없이 OS가 직접 찾는" 표준 경로(EBDA·BIOS ROM 영역 시그니처 스캔,
// ACPI 6.5 §5.2.5.1)를 그대로 구현하면 부트로더 종류와 무관하게 실제
// RSDP를 찾을 수 있다 — Multiboot2 태그는 그 경로가 있을 때의 지름길일
// 뿐이므로, 태그가 없으면(arch_data_addr==0) 이 스캔으로 폴백한다.
#pragma once

#include <cstdint>

namespace arch_x86_64 {

constexpr uint32_t k_max_madt_cpus = 64;  // mm::k_max_cpus와 동일한 골격 상한.

struct madt_result {
    uint64_t lapic_base_phys;  // 기본 0xFEE00000, override 엔트리가 있으면 그 값.
    uint32_t cpu_count;
    uint8_t apic_ids[k_max_madt_cpus];  // Processor Local APIC 엔트리 중 Enabled인 것만.
};

// arch_data_addr(0이면 EBDA/BIOS ROM 스캔으로 자체 검색)에서 RSDP를
// 찾아 MADT까지 파싱한다. 실패(RSDP/MADT를 못 찾음 또는 CPU 엔트리가
// 하나도 없음)하면 false — 호출자는 cpu_count=1(BSP만)으로 취급해야
// 한다(boot.md §2 "토폴로지 정보 없음" 폴백과 같은 정신).
bool find_and_parse_madt(uint64_t arch_data_addr, madt_result& out);

}  // namespace arch_x86_64
