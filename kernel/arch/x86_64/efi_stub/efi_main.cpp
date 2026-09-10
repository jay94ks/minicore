// UEFI 진입 스텁(docs/spec/boot.md §1.2, docs/design/boot-and-drivers.md
// ADR-174). 순수 부트로더 역할이다 — GRUB의 Multiboot2 로더가 이
// 커널에게 하는 일을 UEFI Boot Services로 그대로 재현한다:
//   1. GetMemoryMap+ExitBootServices(펌웨어 소유권 반납, 스펙 요구사항).
//   2. 커널 ELF(이 PE 이미지에 .incbin으로 통째로 심어 둔 것,
//      kernel_blob.S.in)의 PT_LOAD 세그먼트를 p_paddr(물리주소, ld.lld의
//      AT() 지시자가 인코딩)에 그대로 복사한다 — p_memsz>p_filesz인
//      나머지는 0으로 채운다(.bss 관례).
//   3. 커널의 _efi_entry(boot.S, 물리주소는 빌드 시점에 llvm-nm으로
//      뽑아 efi_entry_addr.hpp에 상수로 굳힌다)로 점프한다 — 이후는
//      _start32/_start64와 같은 스택(long mode, 페이징 켜기 전 저지대
//      항등매핑 구성)을 그대로 탄다.
//
// 이 파일은 커널의 나머지 코드(SysV ABI)와 완전히 분리된 별도 실행
// 파일이다 — UEFI 호출 규약(Microsoft x64 ABI)만 efi_types.hpp를 통해
// 이 파일 안에서만 쓰고 밖으로 유출하지 않는다. __builtin_memcpy/memset
// 대신 손으로 쓴 바이트 루프만 쓴다 — 이 빌드(x86_64-unknown-windows
// 타깃, lld-link)는 이번에 처음 시도하는 조합이라, 커널 본체(ELF
// 타깃)에서는 이미 안전하다고 확인된 __builtin_memcpy가 이 새
// 타깃에서도 똑같이 인라인되는지 검증할 방법이 없다 — servers/*가
// 이미 채택한 "가변 길이 memcpy/memset 금지" 관례를 그대로 따르는
// 쪽이 안전하다.
#include "efi_entry_addr.hpp"
#include "efi_types.hpp"

extern "C" {
extern const uint8_t g_kernel_blob_start[];
extern const uint8_t g_kernel_blob_end[];
}

namespace {

// COM1 시리얼(kernel/arch/x86_64/klog_uart.cpp와 완전히 같은 초기화·
// 폴링 로직) — 이 스텁 자신의 진단용. 커널 본체로 넘어간 뒤에는
// kern::klog::init()이 같은 포트를 다시 초기화해 이어받는다.
constexpr uint16_t k_com1_base = 0x3F8;

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_init() {
    outb(k_com1_base + 1, 0x00);
    outb(k_com1_base + 3, 0x80);
    outb(k_com1_base + 0, 0x01);
    outb(k_com1_base + 1, 0x00);
    outb(k_com1_base + 3, 0x03);
    outb(k_com1_base + 2, 0x00);
    outb(k_com1_base + 4, 0x03);
}

void serial_putc(char c) {
    while ((inb(k_com1_base + 5) & 0x20) == 0) {
    }
    outb(k_com1_base + 0, static_cast<uint8_t>(c));
}

void serial_puts(const char* s) {
    while (*s != '\0') {
        serial_putc(*s);
        ++s;
    }
}

void serial_put_hex64(uint64_t v) {
    serial_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nibble = static_cast<uint8_t>((v >> shift) & 0xF);
        char c = static_cast<char>(nibble < 10 ? ('0' + nibble) : ('a' + (nibble - 10)));
        serial_putc(c);
    }
}

void bytes_zero(void* dst, uint64_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < n; ++i) {
        d[i] = 0;
    }
}

void bytes_copy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (uint64_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
}

// ACPI RSDP를 EFI_CONFIGURATION_TABLE에서 찾는다(ACPI 2.0+ GUID
// 우선, 없으면 1.0 GUID) — 못 찾으면 0을 반환한다(그러면 커널이
// acpi.cpp의 EBDA/BIOS ROM 스캔 폴백을 대신 탄다, boot_info_x86_64.cpp
// 주석 참고).
uint64_t find_acpi_rsdp(efi_system_table* st) {
    uint64_t found_v1 = 0;
    for (uint64_t i = 0; i < st->number_of_table_entries; ++i) {
        const efi_configuration_table& e = st->configuration_table[i];
        if (efi_guid_equal(e.vendor_guid, k_acpi_20_table_guid)) {
            return reinterpret_cast<uint64_t>(e.vendor_table);
        }
        if (efi_guid_equal(e.vendor_guid, k_acpi_table_guid)) {
            found_v1 = reinterpret_cast<uint64_t>(e.vendor_table);
        }
    }
    return found_v1;
}

// elf_loader.cpp와 완전히 같은 레이아웃(그 파일의 struct 정의를 그대로
// 옮긴 것 — 두 파일은 서로 다른 빌드 타깃이라 헤더 공유가 불가능해
// 손으로 동기화한다).
struct elf64_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

constexpr uint32_t k_pt_load = 1;
constexpr uint64_t k_page_size = 4096;

// GetMemoryMap 결과를 담을 정적 버퍼 — AllocatePool을 아예 쓰지
// 않으므로(이 스텁이 실제로 필요한 EFI 서비스 목록을 최소로 유지)
// map 항목 수가 이례적으로 많은 환경만 아니면 충분하다.
alignas(8) uint8_t g_memory_map_buffer[16384];

// 커널 ELF의 PT_LOAD 세그먼트를 p_paddr에 그대로 복사한다(GRUB의
// Multiboot2 로더와 동일한 절차) — p_memsz>p_filesz인 나머지는 0으로
// 채운다. AllocatePages(EFI_ALLOCATE_ADDRESS)로 그 물리 범위를 먼저
// 예약해 펌웨어의 다른 할당과 겹치지 않게 한다.
void load_kernel_segments(efi_boot_services* bs, const uint8_t* elf, uint64_t elf_size) {
    elf64_ehdr eh;
    bytes_copy(&eh, elf, sizeof(eh));

    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        elf64_phdr ph;
        bytes_copy(&ph, elf + eh.e_phoff + static_cast<uint64_t>(i) * eh.e_phentsize, sizeof(ph));
        if (ph.p_type != k_pt_load) {
            continue;
        }

        uint64_t pages = (ph.p_memsz + k_page_size - 1) / k_page_size;
        efi_physical_address addr = ph.p_paddr;
        bs->allocate_pages(k_efi_allocate_address, k_efi_loader_data, pages, &addr);

        auto* dst = reinterpret_cast<uint8_t*>(ph.p_paddr);
        bytes_zero(dst, ph.p_memsz);
        bytes_copy(dst, elf + ph.p_offset, ph.p_filesz);
    }
}

}  // namespace

extern "C" __attribute__((ms_abi)) efi_status efi_main(efi_handle image_handle,
                                                         efi_system_table* system_table) {
    serial_init();
    serial_puts("[efi_stub] entered, kernel blob size=");
    uint64_t blob_size =
        static_cast<uint64_t>(g_kernel_blob_end - g_kernel_blob_start);
    serial_put_hex64(blob_size);
    serial_puts("\r\n");

    efi_boot_services* bs = system_table->boot_services;

    load_kernel_segments(bs, g_kernel_blob_start, blob_size);
    serial_puts("[efi_stub] kernel segments loaded\r\n");

    // ACPI RSDP를 찾아 커널의 efi_acpi_rsdp_phys(boot.S)에 직접 써
    // 넣는다 — 세그먼트 복사가 끝난 뒤라야 한다(그 전에 쓰면 .boot.bss
    // 0으로 덮어써지는 복사 단계에 지워진다). Boot Services를 아직
    // 반납하기 전이라 ConfigurationTable 접근이 유효하다.
    uint64_t rsdp = find_acpi_rsdp(system_table);
    serial_puts("[efi_stub] acpi rsdp=");
    serial_put_hex64(rsdp);
    serial_puts("\r\n");
    *reinterpret_cast<volatile uint64_t*>(k_efi_acpi_rsdp_phys_addr) = rsdp;

    uint64_t map_size = sizeof(g_memory_map_buffer);
    uint64_t map_key = 0;
    uint64_t desc_size = 0;
    uint32_t desc_version = 0;
    bs->get_memory_map(&map_size, g_memory_map_buffer, &map_key, &desc_size, &desc_version);

    efi_status st = bs->exit_boot_services(image_handle, map_key);
    if (st != k_efi_success) {
        // 표준 관례 — AllocatePages 등으로 맵이 바뀌었을 수 있어 한 번
        // 더 GetMemoryMap+ExitBootServices를 반복한다.
        map_size = sizeof(g_memory_map_buffer);
        bs->get_memory_map(&map_size, g_memory_map_buffer, &map_key, &desc_size, &desc_version);
        bs->exit_boot_services(image_handle, map_key);
    }

    // Boot Services 반납 이후로는 더 이상 UEFI 호출도, ConOut도 없다 —
    // 이제부터는 물리 메모리에 직접 접근하는 부트 코드다(_start32/64와
    // 같은 전제). 시리얼은 순수 포트 I/O라 계속 쓸 수 있다.
    serial_puts("[efi_stub] exited boot services, jumping to kernel at ");
    serial_put_hex64(k_efi_entry_phys_addr);
    serial_puts("\r\n");

    using efi_entry_fn = void (*)();
    auto entry = reinterpret_cast<efi_entry_fn>(k_efi_entry_phys_addr);
    entry();

    for (;;) {
        asm volatile("hlt");
    }
}
