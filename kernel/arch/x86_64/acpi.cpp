// ACPI RSDP 검색 + MADT 파싱 구현 (acpi.hpp 상단 주석 참고).
#include "acpi.hpp"

#include <mm/phys_map.hpp>

namespace arch_x86_64 {

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
// 전체를 항등 매핑해 두므로(paging_setup.cpp) mm::phys_to_virt로 바로
// 접근 가능하다.
uint64_t scan_for_rsdp() {
    const auto* bda_ebda_seg = static_cast<const uint16_t*>(mm::phys_to_virt(0x40E));
    uint64_t ebda_phys = static_cast<uint64_t>(*bda_ebda_seg) << 4;
    if (ebda_phys != 0) {
        const auto* base = static_cast<const uint8_t*>(mm::phys_to_virt(ebda_phys));
        for (uint32_t off = 0; off < 1024; off += 16) {
            if (looks_like_rsdp(base + off)) {
                return ebda_phys + off;
            }
        }
    }

    constexpr uint64_t k_bios_scan_start = 0xE0000;
    constexpr uint64_t k_bios_scan_end = 0x100000;
    const auto* base = static_cast<const uint8_t*>(mm::phys_to_virt(k_bios_scan_start));
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
    const auto* p = static_cast<const uint8_t*>(mm::phys_to_virt(phys));
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

}  // namespace

bool find_and_parse_madt(uint64_t arch_data_addr, madt_result& out) {
    out.lapic_base_phys = k_default_lapic_base;
    out.cpu_count = 0;

    uint64_t rsdp_phys = arch_data_addr != 0 ? arch_data_addr : scan_for_rsdp();
    if (rsdp_phys == 0) {
        return false;
    }

    const auto* rsdp = static_cast<const uint8_t*>(mm::phys_to_virt(rsdp_phys));
    if (!looks_like_rsdp(rsdp)) {
        // Multiboot2 태그가 준 값이 실제로는 RSDP 시그니처를 만족하지
        // 않는다 — 태그를 신뢰하지 않고 직접 스캔으로 폴백한다.
        rsdp_phys = scan_for_rsdp();
        if (rsdp_phys == 0) {
            return false;
        }
        rsdp = static_cast<const uint8_t*>(mm::phys_to_virt(rsdp_phys));
    }

    uint8_t revision = rsdp[15];
    uint64_t madt_phys = 0;
    if (revision >= 2) {
        uint64_t xsdt_phys = read_u64(rsdp + 24);
        if (xsdt_phys != 0) {
            madt_phys = find_table(xsdt_phys, 8, "APIC");
        }
    }
    if (madt_phys == 0) {
        uint32_t rsdt_phys = read_u32(rsdp + 16);
        if (rsdt_phys == 0) {
            return false;
        }
        madt_phys = find_table(rsdt_phys, 4, "APIC");
    }
    if (madt_phys == 0) {
        return false;
    }

    parse_madt_body(madt_phys, out);
    return out.cpu_count > 0;
}

}  // namespace arch_x86_64
