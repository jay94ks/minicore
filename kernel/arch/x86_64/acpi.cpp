// ACPI RSDP 검색 + MADT 파싱 구현 (acpi.hpp 상단 주석 참고).
#include "acpi.hpp"

#include <mm/phys_map.hpp>

namespace kern::arch::x86_64 {

namespace {

uint32_t read_u32(const uint8_t* p) {
    uint32_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t read_u64(const uint8_t* p) {
    uint64_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return v;
}

bool checksum_ok(const uint8_t* p, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; ++i) {
        sum = static_cast<uint8_t>(sum + p[i]);
    }
    return sum == 0;
}

constexpr uint32_t k_rsdp_v1_size = 20;  // signature[8]+checksum+oem_id[6]+revision+rsdt_address(u32)

bool looks_like_rsdp(const uint8_t* p) {
    // "RSD PTR " (끝에 공백 포함, 8바이트) — ACPI 6.5 §5.2.5.3.
    static constexpr char k_sig[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    for (int i = 0; i < 8; ++i) {
        if (p[i] != static_cast<uint8_t>(k_sig[i])) {
            return false;
        }
    }
    return checksum_ok(p, k_rsdp_v1_size);
}

// EBDA(0x40E의 세그먼트<<4)와 메인 BIOS ROM 영역(0xE0000~0xFFFFF)을
// 16바이트 경계로 스캔한다(ACPI 6.5 §5.2.5.1) — Multiboot2 태그가 RSDP를
// 안 줄 때(이 개발 머신의 QEMU PVH 경로, acpi.hpp 상단 주석)의 표준
// 폴백. 두 스캔 다 물리주소를 그대로 쓴다 — physmap이 이미 512GiB
// 전체를 항등 매핑해 두므로(paging_setup.cpp) kern::mm::phys_to_virt로 바로
// 접근 가능하다.
uint64_t scan_for_rsdp() {
    const auto* bda_ebda_seg = static_cast<const uint16_t*>(kern::mm::phys_to_virt(0x40E));
    uint64_t ebda_phys = static_cast<uint64_t>(*bda_ebda_seg) << 4;
    if (ebda_phys != 0) {
        const auto* base = static_cast<const uint8_t*>(kern::mm::phys_to_virt(ebda_phys));
        for (uint32_t off = 0; off < 1024; off += 16) {
            if (looks_like_rsdp(base + off)) {
                return ebda_phys + off;
            }
        }
    }

    constexpr uint64_t k_bios_scan_start = 0xE0000;
    constexpr uint64_t k_bios_scan_end = 0x100000;
    const auto* base = static_cast<const uint8_t*>(kern::mm::phys_to_virt(k_bios_scan_start));
    for (uint64_t off = 0; off < (k_bios_scan_end - k_bios_scan_start); off += 16) {
        if (looks_like_rsdp(base + off)) {
            return k_bios_scan_start + off;
        }
    }

    return 0;
}

// ACPI SDT 공통 헤더(ACPI 6.5 §5.2.6) — 36바이트. signature/length만
// 이 파일에서 실제로 쓰인다.
struct sdt_view {
    const uint8_t* base;
    uint32_t length;

    bool signature_is(const char* sig4) const {
        for (int i = 0; i < 4; ++i) {
            if (base[i] != static_cast<uint8_t>(sig4[i])) {
                return false;
            }
        }
        return true;
    }
};

sdt_view read_sdt(uint64_t phys) {
    const auto* p = static_cast<const uint8_t*>(kern::mm::phys_to_virt(phys));
    return sdt_view{p, read_u32(p + 4)};
}

// RSDT(32비트 포인터 배열) 또는 XSDT(64비트)에서 signature4와 일치하는
// 첫 테이블의 물리주소를 찾는다. entry_size는 4(RSDT) 또는 8(XSDT).
uint64_t find_table(uint64_t sdt_phys, uint32_t entry_size, const char* signature4) {
    sdt_view sdt = read_sdt(sdt_phys);
    constexpr uint32_t k_header_size = 36;
    if (sdt.length < k_header_size) {
        return 0;
    }
    uint32_t entry_count = (sdt.length - k_header_size) / entry_size;
    const uint8_t* entries = sdt.base + k_header_size;

    for (uint32_t i = 0; i < entry_count; ++i) {
        uint64_t table_phys = (entry_size == 8) ? read_u64(entries + i * 8)
                                                 : static_cast<uint64_t>(read_u32(entries + i * 4));
        if (table_phys == 0) {
            continue;
        }
        sdt_view candidate = read_sdt(table_phys);
        if (candidate.signature_is(signature4)) {
            return table_phys;
        }
    }
    return 0;
}

// RSDP를 찾아(arch_data_addr 우선, 없으면 스캔) RSDT/XSDT를 걸어
// signature4 테이블의 물리주소를 얻는다 — find_and_parse_madt()와
// find_and_parse_srat_slit() 둘 다 이 경로를 공유한다.
uint64_t find_acpi_table(uint64_t arch_data_addr, const char* signature4) {
    uint64_t rsdp_phys = arch_data_addr != 0 ? arch_data_addr : scan_for_rsdp();
    if (rsdp_phys == 0) {
        return 0;
    }

    const auto* rsdp = static_cast<const uint8_t*>(kern::mm::phys_to_virt(rsdp_phys));
    if (!looks_like_rsdp(rsdp)) {
        // Multiboot2 태그가 준 값이 실제로는 RSDP 시그니처를 만족하지
        // 않는다 — 태그를 신뢰하지 않고 직접 스캔으로 폴백한다.
        rsdp_phys = scan_for_rsdp();
        if (rsdp_phys == 0) {
            return 0;
        }
        rsdp = static_cast<const uint8_t*>(kern::mm::phys_to_virt(rsdp_phys));
    }

    uint8_t revision = rsdp[15];
    uint64_t table_phys = 0;
    if (revision >= 2) {
        uint64_t xsdt_phys = read_u64(rsdp + 24);
        if (xsdt_phys != 0) {
            table_phys = find_table(xsdt_phys, 8, signature4);
        }
    }
    if (table_phys == 0) {
        uint32_t rsdt_phys = read_u32(rsdp + 16);
        if (rsdt_phys != 0) {
            table_phys = find_table(rsdt_phys, 4, signature4);
        }
    }
    return table_phys;
}

constexpr uint64_t k_default_lapic_base = 0xFEE00000ull;

constexpr uint8_t k_madt_type_processor_local_apic = 0;
constexpr uint8_t k_madt_type_local_apic_addr_override = 5;
constexpr uint32_t k_madt_processor_enabled = 1u << 0;

void parse_madt_body(uint64_t madt_phys, madt_result& out) {
    sdt_view madt = read_sdt(madt_phys);
    out.lapic_base_phys = static_cast<uint64_t>(read_u32(madt.base + 36));
    out.cpu_count = 0;

    constexpr uint32_t k_madt_header_size = 44;  // SDT(36) + local_apic_address(4) + flags(4)
    uint32_t off = k_madt_header_size;
    while (off + 2 <= madt.length) {
        uint8_t type = madt.base[off];
        uint8_t rec_len = madt.base[off + 1];
        if (rec_len == 0) {
            break;  // 손상된 테이블 — 무한루프 방지.
        }

        if (type == k_madt_type_processor_local_apic && off + 8 <= madt.length) {
            uint8_t apic_id = madt.base[off + 3];
            uint32_t flags = read_u32(madt.base + off + 4);
            if ((flags & k_madt_processor_enabled) != 0 && out.cpu_count < k_max_madt_cpus) {
                out.apic_ids[out.cpu_count++] = apic_id;
            }
        } else if (type == k_madt_type_local_apic_addr_override && off + 12 <= madt.length) {
            out.lapic_base_phys = read_u64(madt.base + off + 4);
        }

        off += rec_len;
    }
}

// SRAT Processor Local APIC/SAPIC Affinity 엔트리(ACPI 6.5 Table 5-46,
// type=0, length=16): proximity domain은 byte2(하위 8비트) +
// byte9~11(상위 24비트, 리틀엔디안)로 나뉘어 있다.
constexpr uint8_t k_srat_type_processor_apic_affinity = 0;
constexpr uint8_t k_srat_type_memory_affinity = 1;
constexpr uint32_t k_srat_processor_enabled = 1u << 0;
constexpr uint32_t k_srat_memory_enabled = 1u << 0;

uint32_t srat_processor_domain(const uint8_t* rec) {
    uint32_t low = rec[2];
    uint32_t high = static_cast<uint32_t>(rec[9]) | (static_cast<uint32_t>(rec[10]) << 8) |
                     (static_cast<uint32_t>(rec[11]) << 16);
    return low | (high << 8);
}

// apic_id -> proximity domain을 먼저 모으고(SRAT 열거 순서, MADT와
// 무관), 그 다음 madt.apic_ids[] 순서(=cpu_id)로 재정렬해
// out.cpu_node[]를 채운다 — boot_info.cpu_node_map_addr의 "인덱스가
// cpu_id" 관례(boot.md §3)를 맞추기 위함이다.
void parse_srat_body(uint64_t srat_phys, const madt_result& madt, srat_slit_result& out) {
    sdt_view srat = read_sdt(srat_phys);

    uint8_t apic_id_to_domain[256] = {};
    bool apic_id_has_domain[256] = {};

    constexpr uint32_t k_srat_header_size = 48;  // SDT(36) + reserved(4) + reserved(8)
    uint32_t off = k_srat_header_size;
    while (off + 2 <= srat.length) {
        uint8_t type = srat.base[off];
        uint8_t rec_len = srat.base[off + 1];
        if (rec_len == 0) {
            break;
        }
        const uint8_t* rec = srat.base + off;

        if (type == k_srat_type_processor_apic_affinity && off + 16 <= srat.length) {
            uint32_t flags = read_u32(rec + 4);
            if ((flags & k_srat_processor_enabled) != 0) {
                uint8_t apic_id = rec[3];
                uint32_t domain = srat_processor_domain(rec);
                if (domain < k_max_numa_nodes) {
                    apic_id_to_domain[apic_id] = static_cast<uint8_t>(domain);
                    apic_id_has_domain[apic_id] = true;
                }
            }
        } else if (type == k_srat_type_memory_affinity && off + 40 <= srat.length) {
            uint32_t flags = read_u32(rec + 28);
            if ((flags & k_srat_memory_enabled) != 0 &&
                out.mem_affinity_count < k_max_memory_affinities) {
                uint32_t domain = read_u32(rec + 2);
                memory_affinity_entry& entry = out.mem_affinities[out.mem_affinity_count++];
                entry.base = read_u64(rec + 8);
                entry.length = read_u64(rec + 16);
                entry.node = domain < k_max_numa_nodes ? domain : 0;
            }
        }

        off += rec_len;
    }

    uint32_t max_domain = 0;
    for (uint32_t i = 0; i < madt.cpu_count; ++i) {
        uint8_t apic_id = madt.apic_ids[i];
        uint32_t domain = apic_id_has_domain[apic_id] ? apic_id_to_domain[apic_id] : 0;
        out.cpu_node[i] = domain;
        if (domain > max_domain) {
            max_domain = domain;
        }
    }
    for (uint32_t i = 0; i < out.mem_affinity_count; ++i) {
        if (out.mem_affinities[i].node > max_domain) {
            max_domain = out.mem_affinities[i].node;
        }
    }
    out.node_count = max_domain + 1;
}

// SLIT(ACPI 6.5 §5.2.17): SDT(36) + locality_count(u64) + 그 뒤
// locality_count*locality_count바이트 행렬. i==j는 항상 10으로
// 정규화한다(스펙이 로컬 거리를 10으로 정의 — QEMU가 실제로 항상
// 이렇게 채우지만, 방어적으로 한 번 더 강제한다).
bool parse_slit_body(uint64_t slit_phys, srat_slit_result& out) {
    sdt_view slit = read_sdt(slit_phys);
    constexpr uint32_t k_slit_header_size = 44;  // SDT(36) + locality_count(8)
    if (slit.length < k_slit_header_size) {
        return false;
    }
    uint64_t locality_count = read_u64(slit.base + 36);
    if (locality_count == 0 || locality_count > k_max_numa_nodes) {
        return false;
    }
    const uint8_t* matrix = slit.base + k_slit_header_size;
    if (k_slit_header_size + locality_count * locality_count > slit.length) {
        return false;
    }

    for (uint32_t i = 0; i < locality_count; ++i) {
        for (uint32_t j = 0; j < locality_count; ++j) {
            out.distance[i][j] = (i == j) ? 10 : matrix[i * locality_count + j];
        }
    }
    return true;
}

void fill_default_distance(srat_slit_result& out) {
    for (uint32_t i = 0; i < k_max_numa_nodes; ++i) {
        for (uint32_t j = 0; j < k_max_numa_nodes; ++j) {
            out.distance[i][j] = (i == j) ? 10 : 20;  // ACPI 관례 기본값.
        }
    }
}

// MCFG(ACPI 6.5 §5.2.6.6) — SDT(36) + reserved(8) = 44바이트 헤더 뒤에
// 16바이트짜리 엔트리(base_address:u64, pci_segment_group:u16,
// start_bus:u8, end_bus:u8, reserved:u32)가 이어진다.
bool parse_mcfg_body(uint64_t mcfg_phys, mcfg_result& out) {
    sdt_view mcfg = read_sdt(mcfg_phys);
    constexpr uint32_t k_header_size = 44;
    constexpr uint32_t k_entry_size = 16;
    if (mcfg.length < k_header_size + k_entry_size) {
        return false;
    }
    uint32_t entry_count = (mcfg.length - k_header_size) / k_entry_size;
    const uint8_t* entries = mcfg.base + k_header_size;
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint8_t* e = entries + i * k_entry_size;
        uint16_t segment = static_cast<uint16_t>(e[8] | (static_cast<uint16_t>(e[9]) << 8));
        if (segment == 0) {
            out.ecam_base_phys = read_u64(e);
            out.ok = true;
            return true;
        }
    }
    return false;
}

// M33(real-libc-syscall-layer.md §M33, ADR-184) — HPET(ACPI 6.5 §5.2.9)
// 헤더: SDT(36) + event_timer_block_id(u32, offset 36) + base_address
// (Generic Address Structure 12바이트, offset 40 — address_space_id(u8)/
// register_bit_width(u8)/register_bit_offset(u8)/reserved(u8)/address(u64),
// 실제 MMIO 물리주소는 이 GAS의 address 필드=offset 44) + hpet_number
// (u8, offset 52) + minimum_tick(u16, offset 53) + page_protection(u8,
// offset 55). 최소 56바이트.
bool parse_hpet_body(uint64_t hpet_phys, hpet_result& out) {
    sdt_view hpet = read_sdt(hpet_phys);
    constexpr uint32_t k_min_length = 56;
    if (hpet.length < k_min_length) {
        return false;
    }
    out.base_phys = read_u64(hpet.base + 44);
    out.ok = true;
    return true;
}

}  // namespace

bool find_and_parse_hpet(uint64_t arch_data_addr, hpet_result& out) {
    out.base_phys = 0;
    out.ok = false;

    uint64_t hpet_phys = find_acpi_table(arch_data_addr, "HPET");
    if (hpet_phys == 0) {
        return false;
    }
    return parse_hpet_body(hpet_phys, out);
}

bool find_and_parse_mcfg(uint64_t arch_data_addr, mcfg_result& out) {
    out.ecam_base_phys = 0;
    out.ok = false;

    uint64_t mcfg_phys = find_acpi_table(arch_data_addr, "MCFG");
    if (mcfg_phys == 0) {
        return false;
    }
    return parse_mcfg_body(mcfg_phys, out);
}

bool find_and_parse_madt(uint64_t arch_data_addr, madt_result& out) {
    out.lapic_base_phys = k_default_lapic_base;
    out.cpu_count = 0;

    uint64_t madt_phys = find_acpi_table(arch_data_addr, "APIC");
    if (madt_phys == 0) {
        return false;
    }

    parse_madt_body(madt_phys, out);
    return out.cpu_count > 0;
}

bool find_and_parse_srat_slit(uint64_t arch_data_addr, const madt_result& madt,
                               srat_slit_result& out) {
    out.mem_affinity_count = 0;
    out.node_count = 1;
    fill_default_distance(out);
    for (uint32_t i = 0; i < madt.cpu_count && i < k_max_madt_cpus; ++i) {
        out.cpu_node[i] = 0;
    }

    uint64_t srat_phys = find_acpi_table(arch_data_addr, "SRAT");
    if (srat_phys == 0) {
        return false;
    }
    parse_srat_body(srat_phys, madt, out);

    uint64_t slit_phys = find_acpi_table(arch_data_addr, "SLIT");
    if (slit_phys != 0) {
        parse_slit_body(slit_phys, out);
    }

    return true;
}

}  // namespace kern::arch::x86_64
