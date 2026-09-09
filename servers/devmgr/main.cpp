// servers/devmgr/main.cpp — 디바이스 매니저: ACPI MCFG 파싱 + PCIe
// ECAM 버스 열거 + 드라이버 등록/매칭 (docs/plan/system-servers-bringup.md
// §M14, docs/spec/pcie.md, ADR-038/039/041).
//
// M14 최소 버전 — 이 파일이 실제로 하는 일:
//   1. initrun이 argv로 넘겨준 boot_info.arch_data_addr(ACPI RSDP
//      물리주소, 0이면 EBDA/BIOS ROM 스캔으로 자체 검색 —
//      kernel/arch/x86_64/acpi.cpp가 이미 커널 자신을 위해 하는 것과
//      **완전히 같은 절차**를 여기서 유저랜드용으로 다시 구현한다,
//      ADR-006/039 — devmgr는 커널 코드를 호출할 수 없다)로 MCFG를
//      찾아 PCI segment 0의 ECAM 베이스를 얻는다.
//   2. bus 0의 device 0~31, function 0만 열거한다(멀티펑션/PCI-PCI
//      브리지 재귀는 M14 범위 밖 — pcie.md §2의 "재귀적으로" 요구를
//      아직 완전히 만족하지 않는 알려진 단순화, QEMU의 virtio-blk/
//      xHCI가 전부 bus 0에 있어 이 정도로 M14 목표를 검증할 수 있다).
//   3. 찾은 장치마다 vendor/device/class를 sys_debug_log로 남긴다.
//   4. sys_ipc_recv 루프를 돌며 드라이버의 REGISTER_DRIVER 요청을
//      받으면 그 기준(vendor:device 정확히 일치, 또는 class code를
//      마스크로 비교)으로 위 장치 테이블을 찾아, 일치하면 그 자리에서
//      BAR0을 배정(ADR-147의 I/O BAR 배정과 같은 절차를 메모리 BAR로
//      일반화, PVH 직접 부팅이라 아무 펌웨어도 이걸 대신해 주지
//      않는다)하고 그 물리주소/크기를 응답으로 돌려준다(pcie.md §4의
//      device_claimed 정보를 register_driver의 **응답**에 바로
//      싣는다 — 핫플러그(ADR-040)가 아직 없어 devmgr가 나중에
//      먼저 말을 거는 별도 경로가 필요 없다).
#include <uapi.hpp>

namespace {

constexpr uint32_t k_own_endpoint_handle = 1;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

uint64_t cstr_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

void debug_log(const char* msg) {
    do_syscall(uapi::k_syscall_debug_log, reinterpret_cast<uint64_t>(msg), cstr_len(msg), 0);
}

// 32바이트 이하 메시지 하나로 로그를 찍는 게 대부분이라 매번 문자열을
// 짜맞추는 대신, 16진수 값 하나를 붙인 고정 포맷 두어 개만 손으로
// 만든다(printf 없음, freestanding).
void debug_log_hex(const char* prefix, uint64_t value) {
    char buf[96];
    uint64_t i = 0;
    for (; prefix[i] != '\0' && i < 60; ++i) {
        buf[i] = prefix[i];
    }
    buf[i++] = '0';
    buf[i++] = 'x';
    bool leading = true;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0xF);
        if (nibble != 0 || !leading || shift == 0) {
            leading = false;
            buf[i++] = static_cast<char>(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10));
        }
    }
    buf[i++] = '\n';
    do_syscall(uapi::k_syscall_debug_log, reinterpret_cast<uint64_t>(buf), i, 0);
}

// ---------- 물리 메모리 창(phys window) ----------
// sys_map_phys는 프로세스마다 고정 가상주소 슬롯 하나를 재사용한다
// (ADR-156) — 그래서 "지금 이 범위가 이미 매핑돼 있으면 재사용,
// 아니면 새로 매핑"하는 아주 작은 캐시 하나로 충분하다. ACPI
// 테이블/ECAM 모두 이 창 하나를 순차적으로 다시 씌워 가며 읽는다.
uint64_t g_window_phys_base = 0;
uint64_t g_window_size = 0;
uint64_t g_window_virt_base = 0;

const uint8_t* map_window(uint64_t phys_addr, uint64_t len) {
    if (g_window_size > 0 && phys_addr >= g_window_phys_base &&
        phys_addr + len <= g_window_phys_base + g_window_size) {
        return reinterpret_cast<const uint8_t*>(g_window_virt_base + (phys_addr - g_window_phys_base));
    }
    uapi::map_phys_request req{};
    req.phys_addr = phys_addr;
    req.size = len;
    uint64_t err = do_syscall(uapi::k_syscall_map_phys, reinterpret_cast<uint64_t>(&req), 0, 0);
    if (err != 0) {
        return nullptr;
    }
    g_window_phys_base = phys_addr;
    g_window_size = len;
    g_window_virt_base = req.out_virt_addr;
    return reinterpret_cast<const uint8_t*>(req.out_virt_addr);
}

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

constexpr uint32_t k_rsdp_v1_size = 20;

bool looks_like_rsdp(const uint8_t* p) {
    static constexpr char k_sig[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    for (int i = 0; i < 8; ++i) {
        if (p[i] != static_cast<uint8_t>(k_sig[i])) {
            return false;
        }
    }
    return checksum_ok(p, k_rsdp_v1_size);
}

// kernel/arch/x86_64/acpi.cpp::scan_for_rsdp()와 완전히 같은 절차
// (ACPI 6.5 §5.2.5.1) — 그 파일 상단 주석 참고. 여기서는
// mm::phys_to_virt 대신 map_window를 쓴다는 점만 다르다.
uint64_t scan_for_rsdp() {
    const uint8_t* bda = map_window(0x400, 0x100);
    if (bda != nullptr) {
        uint16_t ebda_seg;
        __builtin_memcpy(&ebda_seg, bda + 0x0E, sizeof(ebda_seg));
        uint64_t ebda_phys = static_cast<uint64_t>(ebda_seg) << 4;
        if (ebda_phys != 0) {
            const uint8_t* base = map_window(ebda_phys, 1024);
            if (base != nullptr) {
                for (uint32_t off = 0; off < 1024; off += 16) {
                    if (looks_like_rsdp(base + off)) {
                        return ebda_phys + off;
                    }
                }
            }
        }
    }

    constexpr uint64_t k_bios_scan_start = 0xE0000;
    constexpr uint64_t k_bios_scan_size = 0x20000;  // [0xE0000, 0x100000)
    const uint8_t* base = map_window(k_bios_scan_start, k_bios_scan_size);
    if (base != nullptr) {
        for (uint64_t off = 0; off < k_bios_scan_size; off += 16) {
            if (looks_like_rsdp(base + off)) {
                return k_bios_scan_start + off;
            }
        }
    }
    return 0;
}

struct sdt_info {
    uint32_t length = 0;
    bool ok = false;
};

sdt_info read_sdt_header(uint64_t phys, char sig_out[4]) {
    sdt_info info{};
    const uint8_t* p = map_window(phys, 36);
    if (p == nullptr) {
        return info;
    }
    for (int i = 0; i < 4; ++i) {
        sig_out[i] = static_cast<char>(p[i]);
    }
    info.length = read_u32(p + 4);
    info.ok = true;
    return info;
}

bool signature_matches(const char sig[4], const char* sig4) {
    for (int i = 0; i < 4; ++i) {
        if (sig[i] != sig4[i]) {
            return false;
        }
    }
    return true;
}

uint64_t find_table(uint64_t sdt_phys, uint32_t entry_size, const char* signature4) {
    char sig[4];
    sdt_info sdt = read_sdt_header(sdt_phys, sig);
    constexpr uint32_t k_header_size = 36;
    if (!sdt.ok || sdt.length < k_header_size) {
        return 0;
    }
    uint32_t entry_count = (sdt.length - k_header_size) / entry_size;
    for (uint32_t i = 0; i < entry_count; ++i) {
        uint64_t entries_phys = sdt_phys + k_header_size + i * entry_size;
        const uint8_t* entry_ptr = map_window(entries_phys, entry_size);
        if (entry_ptr == nullptr) {
            continue;
        }
        uint64_t table_phys =
            (entry_size == 8) ? read_u64(entry_ptr) : static_cast<uint64_t>(read_u32(entry_ptr));
        if (table_phys == 0) {
            continue;
        }
        char candidate_sig[4];
        sdt_info candidate = read_sdt_header(table_phys, candidate_sig);
        if (candidate.ok && signature_matches(candidate_sig, signature4)) {
            return table_phys;
        }
    }
    return 0;
}

// kernel/arch/x86_64/acpi.cpp::find_acpi_table()와 완전히 같은 절차.
uint64_t find_acpi_table(uint64_t arch_data_addr, const char* signature4) {
    uint64_t rsdp_phys = arch_data_addr != 0 ? arch_data_addr : scan_for_rsdp();
    if (rsdp_phys == 0) {
        return 0;
    }
    const uint8_t* rsdp = map_window(rsdp_phys, k_rsdp_v1_size + 16);
    if (rsdp == nullptr || !looks_like_rsdp(rsdp)) {
        rsdp_phys = scan_for_rsdp();
        if (rsdp_phys == 0) {
            return 0;
        }
        rsdp = map_window(rsdp_phys, k_rsdp_v1_size + 16);
        if (rsdp == nullptr) {
            return 0;
        }
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
            table_phys = find_table(static_cast<uint64_t>(rsdt_phys), 4, signature4);
        }
    }
    return table_phys;
}

// MCFG(ACPI 6.5 §5.2.6.6) — kernel/arch/x86_64/acpi.cpp::parse_mcfg_body()
// 와 완전히 같은 레이아웃.
bool find_mcfg_ecam_base(uint64_t arch_data_addr, uint64_t& out_ecam_base) {
    uint64_t mcfg_phys = find_acpi_table(arch_data_addr, "MCFG");
    if (mcfg_phys == 0) {
        return false;
    }
    char sig[4];
    sdt_info mcfg = read_sdt_header(mcfg_phys, sig);
    constexpr uint32_t k_header_size = 44;
    constexpr uint32_t k_entry_size = 16;
    if (!mcfg.ok || mcfg.length < k_header_size + k_entry_size) {
        return false;
    }
    uint32_t entry_count = (mcfg.length - k_header_size) / k_entry_size;
    for (uint32_t i = 0; i < entry_count; ++i) {
        uint64_t entry_phys = mcfg_phys + k_header_size + i * k_entry_size;
        const uint8_t* e = map_window(entry_phys, k_entry_size);
        if (e == nullptr) {
            continue;
        }
        uint16_t segment = static_cast<uint16_t>(e[8] | (static_cast<uint16_t>(e[9]) << 8));
        if (segment == 0) {
            out_ecam_base = read_u64(e);
            return true;
        }
    }
    return false;
}

// ---------- PCI ECAM ----------
constexpr uint32_t k_pci_offset_vendor = 0x00;
constexpr uint32_t k_pci_offset_class = 0x08;   // revision_id(1)+prog_if(1)+subclass(1)+class(1).
constexpr uint32_t k_pci_offset_header_type = 0x0E;
constexpr uint32_t k_pci_offset_command = 0x04;
constexpr uint32_t k_pci_offset_bar0 = 0x10;
constexpr uint32_t k_pci_command_mem_space_enable = 1u << 1;

uint64_t ecam_config_addr(uint64_t ecam_base, uint32_t bus, uint32_t device, uint32_t function) {
    return ecam_base + ((static_cast<uint64_t>(bus) << 20) | (static_cast<uint64_t>(device) << 15) |
                         (static_cast<uint64_t>(function) << 12));
}

struct pci_device_entry {
    bool valid = false;
    uint32_t bus = 0;
    uint32_t device = 0;
    uint32_t function = 0;
    uint16_t vendor_id = 0;
    uint16_t device_id = 0;
    uint32_t class_code = 0;  // (base_class<<16)|(subclass<<8)|prog_if.
};

constexpr uint32_t k_max_pci_devices = 32;
pci_device_entry g_devices[k_max_pci_devices];
uint32_t g_device_count = 0;
uint64_t g_ecam_base = 0;

void enumerate_pci_bus0(uint64_t ecam_base) {
    for (uint32_t device = 0; device < 32 && g_device_count < k_max_pci_devices; ++device) {
        uint64_t cfg = ecam_config_addr(ecam_base, 0, device, 0);
        const uint8_t* p = map_window(cfg, 64);
        if (p == nullptr) {
            continue;
        }
        uint16_t vendor_id = static_cast<uint16_t>(p[k_pci_offset_vendor] |
                                                    (static_cast<uint16_t>(p[k_pci_offset_vendor + 1]) << 8));
        if (vendor_id == 0xFFFF) {
            continue;  // 장치 없음.
        }
        uint16_t device_id = static_cast<uint16_t>(p[k_pci_offset_vendor + 2] |
                                                     (static_cast<uint16_t>(p[k_pci_offset_vendor + 3]) << 8));
        uint32_t class_code = (static_cast<uint32_t>(p[k_pci_offset_class + 3]) << 16) |
                               (static_cast<uint32_t>(p[k_pci_offset_class + 2]) << 8) |
                               static_cast<uint32_t>(p[k_pci_offset_class + 1]);

        pci_device_entry& entry = g_devices[g_device_count++];
        entry.valid = true;
        entry.bus = 0;
        entry.device = device;
        entry.function = 0;
        entry.vendor_id = vendor_id;
        entry.device_id = device_id;
        entry.class_code = class_code;

        debug_log("[devmgr] pci device found\n");
        debug_log_hex("[devmgr]   vendor=", vendor_id);
        debug_log_hex("[devmgr]   device=", device_id);
        debug_log_hex("[devmgr]   class=", class_code);

        (void)k_pci_offset_header_type;  // 멀티펑션/브리지 재귀는 M14 범위 밖(상단 주석).
    }
}

// ADR-147의 I/O BAR 배정을 메모리 BAR로 일반화한 것 — pci_bringup.cpp
// 상단 주석과 같은 이유(PVH 직접 부팅이라 아무 펌웨어도 이걸 대신해
// 주지 않는다). 이 최소 devmgr는 32비트 메모리 BAR만 다룬다(M14가
// 실제로 만나는 xHCI가 그렇다 — 64비트 BAR는 M14 범위 밖).
bool assign_memory_bar0(uint64_t ecam_base, uint32_t bus, uint32_t device, uint32_t function,
                         uint64_t assign_phys_base, uint64_t& out_bar_phys, uint64_t& out_bar_size) {
    uint64_t cfg = ecam_config_addr(ecam_base, bus, device, function);
    const uint8_t* view = map_window(cfg, 64);
    if (view == nullptr) {
        return false;
    }
    uint32_t bar0 = read_u32(view + k_pci_offset_bar0);
    if ((bar0 & 0x1) != 0) {
        return false;  // I/O 공간 BAR — 이 함수는 메모리 BAR만 다룬다.
    }

    // map_window의 창이 읽기 전용 관찰용으로 쓰였으니, 쓰기 왕복은
    // 매번 새로 map_window(같은 범위)를 불러 volatile 포인터로 한다 —
    // 쓰기 후 읽기가 실제 하드웨어 상태를 반영해야 하므로 캐시된
    // 창이라도 다시 조회해 안전하게 접근한다.
    auto cfg_ptr32 = [&](uint32_t offset) -> volatile uint32_t* {
        const uint8_t* p = map_window(cfg, 64);
        return reinterpret_cast<volatile uint32_t*>(const_cast<uint8_t*>(p) + offset);
    };
    auto cfg_ptr16 = [&](uint32_t offset) -> volatile uint16_t* {
        const uint8_t* p = map_window(cfg, 64);
        return reinterpret_cast<volatile uint16_t*>(const_cast<uint8_t*>(p) + offset);
    };

    *cfg_ptr32(k_pci_offset_bar0) = 0xFFFFFFFFu;
    uint32_t size_mask_raw = *cfg_ptr32(k_pci_offset_bar0);
    uint32_t size_mask = size_mask_raw & ~0xFu;  // 하위 4비트(공간/타입/prefetchable) 제외.
    uint32_t size = (~size_mask) + 1;
    if (size == 0) {
        *cfg_ptr32(k_pci_offset_bar0) = bar0;
        return false;
    }

    uint64_t assigned = (assign_phys_base + size - 1) & ~(static_cast<uint64_t>(size) - 1);
    *cfg_ptr32(k_pci_offset_bar0) = static_cast<uint32_t>(assigned);

    uint16_t command = *cfg_ptr16(k_pci_offset_command);
    *cfg_ptr16(k_pci_offset_command) =
        static_cast<uint16_t>(command | k_pci_command_mem_space_enable);

    out_bar_phys = assigned;
    out_bar_size = size;
    return true;
}

// M14가 실제로 배정하는 장치는 최대 하나(xHCI)뿐이라 고정 주소 하나로
// 충분하다 — QEMU q35의 PCI MMIO 홀(전형적으로 0xC0000000 부근) 안의
// 임의 정렬 주소. 여러 장치를 배정해야 하는 시점(devmgr가 진짜
// 여러 드라이버를 다루게 될 때)에는 실제 공간 할당기가 필요해진다
// (알려진 단순화 — ADR-147의 "고정 I/O 베이스"와 같은 정신).
constexpr uint64_t k_assigned_mmio_base = 0xE0000000ull;

// ---------- 드라이버 등록 프로토콜 (pcie.md §4, 이 마일스톤 한정 최소 버전) ----------
constexpr uint32_t k_op_register_driver = 1;
constexpr uint64_t k_match_mode_vendor_device = 0;
constexpr uint64_t k_match_mode_class_code = 1;
constexpr uint64_t k_status_ok = 0;
constexpr uint64_t k_status_not_found = 1;

void handle_register_driver(const uapi::message& in, uapi::message& out) {
    uint64_t mode = in.regs[0];
    uint64_t match_a = in.regs[1];
    uint64_t match_mask = in.regs[2];

    for (uint32_t i = 0; i < g_device_count; ++i) {
        const pci_device_entry& d = g_devices[i];
        bool matched = false;
        if (mode == k_match_mode_vendor_device) {
            uint64_t vd = (static_cast<uint64_t>(d.vendor_id) << 16) | d.device_id;
            matched = (vd == match_a);
        } else if (mode == k_match_mode_class_code) {
            matched = ((d.class_code & match_mask) == (match_a & match_mask));
        }
        if (!matched) {
            continue;
        }

        uint64_t bar_phys = 0;
        uint64_t bar_size = 0;
        if (!assign_memory_bar0(g_ecam_base, d.bus, d.device, d.function, k_assigned_mmio_base,
                                 bar_phys, bar_size)) {
            continue;
        }
        out.regs[0] = k_status_ok;
        out.regs[1] = bar_phys;
        out.regs[2] = bar_size;
        out.regs[3] = (static_cast<uint64_t>(d.bus) << 16) | (static_cast<uint64_t>(d.device) << 8) |
                      d.function;
        return;
    }

    out.regs[0] = k_status_not_found;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void* argv) {
    uint64_t arch_data_addr = 0;
    if (argv != nullptr) {
        __builtin_memcpy(&arch_data_addr, argv, sizeof(arch_data_addr));
    }

    bool have_ecam = find_mcfg_ecam_base(arch_data_addr, g_ecam_base);
    debug_log_hex("[devmgr] mcfg ecam_base=", have_ecam ? g_ecam_base : 0xFFFFFFFFFFFFFFFFull);

    if (have_ecam) {
        enumerate_pci_bus0(g_ecam_base);
    }
    debug_log_hex("[devmgr] device_count=", g_device_count);

    for (;;) {
        uapi::message in{};
        uint64_t recv_err = do_syscall(uapi::k_syscall_ipc_recv, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in), 0);
        uapi::message out{};
        if (recv_err == 0) {
            out.label = in.label;
            if (in.label == k_op_register_driver) {
                handle_register_driver(in, out);
            }
        }
        do_syscall(uapi::k_syscall_ipc_reply, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
