// boot_info: 커널 코어와 initrun이 공유하는 유일한 부팅 관련 ABI
// (docs/spec/boot.md §3). 커널 코어는 이 구조체만 알고 Multiboot2/UEFI/FDT의
// 존재 자체를 모른다(ADR-002 HAL 경계) — 각 arch 계층이 자신의 부트
// 프로토콜을 파싱해 이 구조체로 변환한다.
#pragma once

#include <cstdint>

namespace boot {

constexpr uint64_t k_boot_info_magic = 0x4D434249ull;  // "MCBI"
constexpr uint32_t k_boot_info_version = 2;            // NUMA 토폴로지 필드 포함(ADR-035/036)

constexpr uint32_t k_region_usable = 0;
constexpr uint32_t k_region_reserved = 1;
constexpr uint32_t k_region_acpi_reclaimable = 2;
constexpr uint32_t k_region_kernel_image = 3;
constexpr uint32_t k_region_initrd_image = 4;

struct memory_region {
    uint64_t base;
    uint64_t length;
    uint32_t type;     // k_region_* 중 하나
    uint32_t node_id;  // NUMA 노드 번호(ADR-036). 토폴로지 정보 없으면 0.
};

// cpu_id(0..cpu_count-1) → NUMA 노드 번호는 cpu_node_map_addr가 가리키는
// uint32_t[cpu_count] 배열의 인덱스가 cpu_id다(ADR-034/036).
struct boot_info {
    uint64_t magic;
    uint32_t version;
    uint32_t cpu_count;  // M1~M8 실행 환경에서는 1(ADR-035).

    uint64_t memory_map_addr;  // memory_region 배열의 물리주소
    uint32_t memory_map_count;
    uint32_t numa_node_count;  // 토폴로지 정보 없으면 1.

    uint64_t cpu_node_map_addr;  // 토폴로지 정보 없으면 0(모든 cpu_id를 노드 0으로 취급).

    uint64_t initrd_addr;  // 물리주소, 없으면 0
    uint64_t initrd_size;

    uint64_t cmdline_addr;  // NUL 종료 문자열 물리주소, 없으면 0

    uint64_t arch_data_addr;  // arch별 부가정보(ACPI RSDP 등), 없으면 0
};

// klog으로 boot_info 내용을 덤프한다 — arch 독립(ADR-002), kernel/core에서
// 구현한다. docs/plan/kernel-bootstrap.md M2의 검증 산출물("메모리맵
// 항목 수·initrd 위치를 시리얼 콘솔에 덤프")이 이 함수의 출력이다.
// `regions`는 이미 접근 가능한(가상주소) 포인터여야 한다 — info.memory_map_addr
// 자체는 물리주소이므로 이 함수가 직접 역참조하지 않는다(arch의
// phys_to_virt가 필요해질 것이므로 HAL 경계를 지키기 위해 호출자가
// 미리 넘긴다).
void dump(const char* tag, const boot_info& info, const memory_region* regions);

}  // namespace boot
