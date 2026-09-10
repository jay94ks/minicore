// x86_64 Multiboot2 태그 파서 (docs/spec/boot.md §2~3, ADR-002 HAL 경계 —
// 이 파일만 Multiboot2의 존재를 안다. kernel_main이 결과로 얻는
// boot::boot_info는 arch 독립이다).
#include "boot_info_x86_64.hpp"
#include "memory_layout.hpp"
#include "smp.hpp"

#include <mm/phys_map.hpp>

namespace kern::arch::x86_64 {

namespace {

constexpr uint32_t k_mb2_tag_end = 0;
constexpr uint32_t k_mb2_tag_cmdline = 1;
constexpr uint32_t k_mb2_tag_module = 3;
constexpr uint32_t k_mb2_tag_memory_map = 6;
constexpr uint32_t k_mb2_tag_acpi_old_rsdp = 14;
constexpr uint32_t k_mb2_tag_acpi_new_rsdp = 15;

constexpr uint32_t k_mb2_mem_available = 1;
constexpr uint32_t k_mb2_mem_reserved = 2;
constexpr uint32_t k_mb2_mem_acpi_reclaimable = 3;
constexpr uint32_t k_mb2_mem_acpi_nvs = 4;
constexpr uint32_t k_mb2_mem_defective = 5;

// <cstring>은 ADR-010 허용 목록에 없다 — __builtin_memcpy는 헤더 없이
// 항상 쓸 수 있는 컴파일러 내장이라 정렬 보장이 없는 태그 필드를
// 안전하게 읽는 데 쓴다.
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

void write_u32(uint8_t* p, uint32_t v) {
    __builtin_memcpy(p, &v, sizeof(v));
}

void write_u64(uint8_t* p, uint64_t v) {
    __builtin_memcpy(p, &v, sizeof(v));
}

uint32_t translate_region_type(uint32_t mb2_type) {
    switch (mb2_type) {
        case k_mb2_mem_available:
            return boot::k_region_usable;
        case k_mb2_mem_acpi_reclaimable:
            return boot::k_region_acpi_reclaimable;
        case k_mb2_mem_reserved:
        case k_mb2_mem_acpi_nvs:
        case k_mb2_mem_defective:
        default:
            return boot::k_region_reserved;
    }
}

extern "C" {
extern const char _image_end[];  // link.ld export (higher-half 가상주소).
}

constexpr uint32_t k_max_memory_regions = 64;
boot::memory_region g_memory_regions[k_max_memory_regions];

// 커널 자신과 initrd가 차지한 물리 범위를 memory_map에 명시적으로
// 추가한다 — 안 하면 M3의 물리 메모리 할당자가 raw 메모리맵만 보고
// 이 범위를 "쓸 수 있음"으로 오해해 실행 중인 커널 위에 할당할 수
// 있다(boot.md §3의 k_region_kernel_image/k_region_initrd_image가
// 바로 이 문제를 위해 있다).
void append_owned_regions(uint32_t& region_count, const boot::boot_info& info) {
    if (region_count < k_max_memory_regions) {
        boot::memory_region& r = g_memory_regions[region_count++];
        r.base = k_kernel_phys_base;
        r.length = image_virt_to_phys(_image_end) - k_kernel_phys_base;
        r.type = boot::k_region_kernel_image;
        r.node_id = 0;
    }
    if (info.initrd_addr != 0 && region_count < k_max_memory_regions) {
        boot::memory_region& r = g_memory_regions[region_count++];
        r.base = info.initrd_addr;
        r.length = info.initrd_size;
        r.type = boot::k_region_initrd_image;
        r.node_id = 0;
    }

    // M10(ADR-055) — AP 트램폴린 스크래치 페이지(smp.hpp::k_ap_trampoline_phys)
    // 도 커널 자신/initrd와 같은 이유로 물리 할당자에서 영구히 빼야
    // 한다 — smp.cpp가 부팅 극초기에 이 페이지에 트램폴린 코드를 써
    // 두고, 이후 AP가 언제든 그 코드를 다시 실행할 수 있어 kern::mm::alloc_pages가
    // 이 페이지를 다른 용도로 내주면 안 된다. k_region_kernel_image를
    // 그대로 재사용한다(새 type 값을 추가하지 않는다) — 이 영역도
    // "커널이 이미 소유한 물리 범위"라는 점에서 실제 커널 이미지와
    // 배제 처리 방식이 정확히 같다(page_allocator.cpp init()의 exclusion
    // 목록이 이 type 값을 그대로 걸러낸다).
    if (region_count < k_max_memory_regions) {
        boot::memory_region& r = g_memory_regions[region_count++];
        r.base = kern::arch::x86_64::k_ap_trampoline_phys;
        r.length = kern::arch::x86_64::k_ap_trampoline_size;
        r.type = boot::k_region_kernel_image;
        r.node_id = 0;
    }
}

// M11(ADR-036) — build_numa_boot_info()가 채우는 cpu_id→node 배열.
// g_memory_regions와 같은 이유로 재호출 시 덮어써지는 정적 저장소다.
uint32_t g_cpu_node_map[k_max_madt_cpus];

}  // namespace

boot::boot_info build_boot_info(uint32_t multiboot_magic, uint32_t multiboot_info_phys,
                                 const boot::memory_region** out_regions) {
    boot::boot_info info{};
    info.magic = boot::k_boot_info_magic;
    info.version = boot::k_boot_info_version;
    info.cpu_count = 1;        // M1~M8: BSP 단일 코어(ADR-035)
    info.numa_node_count = 1;  // 토폴로지 정보 없음(ADR-034/036)
    info.cpu_node_map_addr = 0;

    uint32_t region_count = 0;
    *out_regions = g_memory_regions;

    if (multiboot_magic != k_multiboot2_bootloader_magic) {
        // Multiboot2로 부팅되지 않았다(예: QEMU PVH 개발 경로, ADR-114) —
        // 파싱할 것이 없다.
        info.memory_map_addr = image_virt_to_phys(g_memory_regions);
        info.memory_map_count = 0;
        return info;
    }

    const uint8_t* mb2 = static_cast<const uint8_t*>(kern::mm::phys_to_virt(multiboot_info_phys));
    uint32_t total_size = read_u32(mb2);

    const uint8_t* p = mb2 + 8;  // total_size(4) + reserved(4) 건너뜀
    const uint8_t* end = mb2 + total_size;

    while (p + 8 <= end) {
        uint32_t tag_type = read_u32(p);
        uint32_t tag_size = read_u32(p + 4);
        if (tag_type == k_mb2_tag_end) {
            break;
        }

        switch (tag_type) {
            case k_mb2_tag_memory_map: {
                uint32_t entry_size = read_u32(p + 8);
                const uint8_t* entry = p + 16;
                const uint8_t* mmap_end = p + tag_size;
                while (entry + entry_size <= mmap_end && region_count < k_max_memory_regions) {
                    boot::memory_region& r = g_memory_regions[region_count++];
                    r.base = read_u64(entry);
                    r.length = read_u64(entry + 8);
                    r.type = translate_region_type(read_u32(entry + 16));
                    r.node_id = 0;
                    entry += entry_size;
                }
                break;
            }
            case k_mb2_tag_module: {
                if (info.initrd_addr == 0) {
                    uint32_t mod_start = read_u32(p + 8);
                    uint32_t mod_end = read_u32(p + 12);
                    info.initrd_addr = mod_start;
                    info.initrd_size = mod_end - mod_start;
                }
                break;
            }
            case k_mb2_tag_cmdline: {
                info.cmdline_addr = kern::mm::virt_to_phys(p + 8);
                break;
            }
            case k_mb2_tag_acpi_old_rsdp:
            case k_mb2_tag_acpi_new_rsdp: {
                if (info.arch_data_addr == 0) {
                    info.arch_data_addr = kern::mm::virt_to_phys(p + 8);
                }
                break;
            }
            default:
                break;
        }

        p += (tag_size + 7) & ~7u;  // 다음 태그는 8바이트 경계
    }

    append_owned_regions(region_count, info);

    info.memory_map_addr = image_virt_to_phys(g_memory_regions);
    info.memory_map_count = region_count;
    return info;
}

namespace {

// build_selftest_blob()이 여기 채운 바이트를 build_boot_info()가 그대로
// 파싱한다 — 실제 GRUB Multiboot2 파서 경로와 동일한 코드를 탄다.
alignas(8) uint8_t g_selftest_blob[512];

uint8_t* write_tag_header(uint8_t* p, uint32_t type, uint32_t size) {
    write_u32(p, type);
    write_u32(p + 4, size);
    return p + 8;
}

uint8_t* align8(uint8_t* p, const uint8_t* base) {
    auto offset = static_cast<uint32_t>(p - base);
    uint32_t padded = (offset + 7) & ~7u;
    return const_cast<uint8_t*>(base) + padded;
}

uint32_t build_selftest_blob() {
    uint8_t* base = g_selftest_blob;
    uint8_t* p = base + 8;  // total_size(4)+reserved(4)는 마지막에 채운다

    // 메모리맵 태그: 가짜 영역 2개(usable, reserved).
    uint8_t* mmap_tag = p;
    p = write_tag_header(p, k_mb2_tag_memory_map, 0);  // size는 되채운다
    write_u32(p, 24);
    p += 4;  // entry_size
    write_u32(p, 0);
    p += 4;  // entry_version
    struct fake_entry {
        uint64_t base;
        uint64_t length;
        uint32_t type;
        uint32_t reserved;
    };
    const fake_entry entries[2] = {
        {0x0000000000000000ull, 0x000000000009FC00ull, k_mb2_mem_available, 0},
        {0x0000000000100000ull, 0x0000000007F00000ull, k_mb2_mem_available, 0},
    };
    for (const fake_entry& e : entries) {
        write_u64(p, e.base);
        p += 8;
        write_u64(p, e.length);
        p += 8;
        write_u32(p, e.type);
        p += 4;
        write_u32(p, e.reserved);
        p += 4;
    }
    write_u32(mmap_tag + 4, static_cast<uint32_t>(p - mmap_tag));
    p = align8(p, base);

    // 모듈 태그: 가짜 initrd.
    uint8_t* mod_tag = p;
    p = write_tag_header(p, k_mb2_tag_module, 0);
    write_u32(p, 0x01000000);
    p += 4;  // mod_start
    write_u32(p, 0x01100000);
    p += 4;  // mod_end
    const char name[] = "initrd";
    __builtin_memcpy(p, name, sizeof(name));
    p += sizeof(name);
    write_u32(mod_tag + 4, static_cast<uint32_t>(p - mod_tag));
    p = align8(p, base);

    // 커맨드라인 태그.
    uint8_t* cmd_tag = p;
    p = write_tag_header(p, k_mb2_tag_cmdline, 0);
    const char cmdline[] = "selftest=1";
    __builtin_memcpy(p, cmdline, sizeof(cmdline));
    p += sizeof(cmdline);
    write_u32(cmd_tag + 4, static_cast<uint32_t>(p - cmd_tag));
    p = align8(p, base);

    // 끝 태그.
    p = write_tag_header(p, k_mb2_tag_end, 8);

    uint32_t total_size = static_cast<uint32_t>(p - base);
    write_u32(base, total_size);
    write_u32(base + 4, 0);  // reserved
    return total_size;
}

}  // namespace

boot::boot_info run_boot_info_self_test(const boot::memory_region** out_regions) {
    build_selftest_blob();
    uint64_t phys = image_virt_to_phys(g_selftest_blob);
    return build_boot_info(k_multiboot2_bootloader_magic, static_cast<uint32_t>(phys),
                            out_regions);
}

boot::boot_info build_numa_boot_info(const madt_result& madt, const srat_slit_result& srat,
                                      const boot::memory_region** out_regions,
                                      const uint32_t** out_cpu_node_map) {
    boot::boot_info info{};
    info.magic = boot::k_boot_info_magic;
    info.version = boot::k_boot_info_version;
    info.cpu_count = madt.cpu_count;
    info.numa_node_count = srat.node_count;

    for (uint32_t i = 0; i < madt.cpu_count && i < k_max_madt_cpus; ++i) {
        g_cpu_node_map[i] = srat.cpu_node[i];
    }
    info.cpu_node_map_addr = image_virt_to_phys(g_cpu_node_map);
    *out_cpu_node_map = g_cpu_node_map;

    // initrd_addr을 일부러 0으로 둔다 — 실제 initrd 바이트는 커널
    // 이미지의 .rodata에 직접 임베딩돼 있어(ADR-119) 아래
    // append_owned_regions()의 "kernel_image" exclusion에 이미
    // 포함된다. 별도 initrd exclusion은 self-test fixture처럼 "가짜
    // 겹침"을 일부러 만드는 경우에만 의미가 있다(run_boot_info_self_test
    // 참고) — 이 NUMA 경로는 실제 주소만 다루므로 필요 없다.
    uint32_t region_count = 0;
    for (uint32_t i = 0; i < srat.mem_affinity_count && region_count < k_max_memory_regions; ++i) {
        boot::memory_region& r = g_memory_regions[region_count++];
        r.base = srat.mem_affinities[i].base;
        r.length = srat.mem_affinities[i].length;
        r.type = boot::k_region_usable;
        r.node_id = srat.mem_affinities[i].node;
    }
    append_owned_regions(region_count, info);

    *out_regions = g_memory_regions;
    info.memory_map_addr = image_virt_to_phys(g_memory_regions);
    info.memory_map_count = region_count;
    return info;
}

}  // namespace kern::arch::x86_64
