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

// M11(smp-fpu-bringup.md §M11, ADR-036) — SRAT(Static Resource
// Affinity Table)+SLIT(System Locality Information Table) 파싱. RSDP
// 검색은 find_and_parse_madt()와 완전히 같은 경로(arch_data_addr 우선,
// 없으면 EBDA/BIOS ROM 스캔)를 재사용한다 — QEMU가 `-numa` 옵션을 줬을
// 때만 이 두 테이블이 실제로 존재한다(옵션 없으면 아래에서 false).
//
// 노드 번호 부여 규칙: SRAT의 "proximity domain" 값을 그대로 노드
// 번호로 쓴다(재압축하지 않는다) — QEMU가 `-numa node,nodeid=N`으로
// 지정한 값이 SRAT에 그대로 실리는 것을 실측으로 확인했다. 값이
// k_max_numa_nodes를 넘는 도메인은 무시한다(골격 상한, mm::k_max_numa_nodes
// 와 동일).
constexpr uint32_t k_max_numa_nodes = 8;
constexpr uint32_t k_max_memory_affinities = 32;

struct memory_affinity_entry {
    uint64_t base;
    uint64_t length;
    uint32_t node;
};

struct srat_slit_result {
    uint32_t node_count;  // 관측된 최대 proximity domain + 1.

    // cpu_node[i]는 madt_result::apic_ids[i]의 노드 번호다(같은 인덱스
    // 순서 — boot_info.cpu_node_map_addr의 "cpu_id=배열 인덱스" 관례와
    // 맞추기 위해 madt 열거 순서를 그대로 따른다). SRAT에 없는 CPU는
    // 노드 0으로 취급한다.
    uint32_t cpu_node[k_max_madt_cpus];

    memory_affinity_entry mem_affinities[k_max_memory_affinities];
    uint32_t mem_affinity_count;

    // distance[i][j] — SLIT이 있으면 그 값, 없으면 ACPI 관례 기본값
    // (같은 노드=10, 다른 노드=20)로 채운다. i==j는 항상 10(SLIT
    // 유무와 무관 — ACPI 6.5 §5.2.17이 로컬 거리를 10으로 정의).
    uint8_t distance[k_max_numa_nodes][k_max_numa_nodes];
};

// madt는 cpu_node[] 인덱스 순서(=cpu_id)를 madt.apic_ids와 맞추기 위해
// 필요하다. SRAT을 못 찾으면 false — 호출자는 노드 1개(전부 노드
// 0)로 취급해야 한다(boot.md §2 "토폴로지 정보 없음" 폴백과 같은
// 정신). SRAT은 찾았지만 SLIT은 없는 경우는 실패가 아니다 — 기본
// 거리값으로 distance를 채우고 true를 반환한다.
bool find_and_parse_srat_slit(uint64_t arch_data_addr, const madt_result& madt,
                               srat_slit_result& out);

// M12(system-servers-bringup.md §M12, ADR-131/147) — MCFG(PCI Express
// Memory-mapped Configuration space table, ACPI 6.5 §5.2.6.6) 파싱.
// initrun의 임베디드 virtio-blk 클라이언트가 부트 디바이스(고정
// BDF, boot_info.boot_device)의 설정공간을 ECAM으로 읽으려면 이
// 세그먼트 0의 ECAM 베이스 물리주소가 필요하다. RSDP 검색은
// find_and_parse_madt()와 완전히 같은 경로를 재사용한다.
struct mcfg_result {
    uint64_t ecam_base_phys;  // PCI segment 0의 ECAM 베이스. 실패하면 의미 없음.
    bool ok;
};

// segment 0 엔트리를 찾으면 true — 없으면(MCFG 자체가 없거나 segment
// 0 엔트리가 없음) false. ECAM 방식만 다룬다(ADR-038의 "ECAM 우선"
// 결정 재사용) — 레거시 0xCF8/0xCFC 폴백은 이 최소 클라이언트의
// 범위 밖이다(§근거: 부트스트랩 1회성 전용, QEMU q35는 항상 MCFG를
// 제공한다).
bool find_and_parse_mcfg(uint64_t arch_data_addr, mcfg_result& out);

}  // namespace arch_x86_64
