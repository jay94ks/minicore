// UEFI 스펙의 최소 부분집합(EFI_SYSTEM_TABLE/EFI_BOOT_SERVICES 중
// GetMemoryMap/AllocatePages/ExitBootServices와 ACPI RSDP를 찾는 데
// 필요한 EFI_CONFIGURATION_TABLE만) — docs/spec/boot.md §1.2.
//
// UEFI 함수는 전부 Microsoft x64 호출 규약(ms_abi)이다 — 이 커널의
// 나머지 코드는 SysV(x86_64-elf 기본값)라 이 파일 밖으로 절대 유출되면
// 안 된다(efi_main.cpp만 이 헤더를 쓴다).
#pragma once

#include <cstdint>

extern "C" {

using efi_status = uint64_t;
using efi_handle = void*;
using efi_physical_address = uint64_t;

struct efi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

// UEFI 스펙 표 그대로의 필드 순서 — GetMemoryMap/AllocatePages/
// ExitBootServices 외에는 실제 시그니처를 알 필요가 없어(호출하지
// 않는다) void* 자리표시자로 채운다. **개수와 순서**만 정확하면
// 오프셋이 맞는다(전부 8바이트 포인터 슬롯).
using efi_allocate_pages_fn = efi_status(__attribute__((ms_abi)) *)(
    uint32_t type, uint32_t memory_type, uint64_t pages, efi_physical_address* memory);

using efi_get_memory_map_fn = efi_status(__attribute__((ms_abi)) *)(
    uint64_t* memory_map_size, void* memory_map, uint64_t* map_key, uint64_t* descriptor_size,
    uint32_t* descriptor_version);

using efi_exit_boot_services_fn = efi_status(__attribute__((ms_abi)) *)(efi_handle image_handle,
                                                                         uint64_t map_key);

struct efi_boot_services {
    efi_table_header hdr;

    void* raise_tpl;
    void* restore_tpl;

    efi_allocate_pages_fn allocate_pages;
    void* free_pages;
    efi_get_memory_map_fn get_memory_map;
    void* allocate_pool;
    void* free_pool;

    void* create_event;
    void* set_timer;
    void* wait_for_event;
    void* signal_event;
    void* close_event;
    void* check_event;

    void* install_protocol_interface;
    void* reinstall_protocol_interface;
    void* uninstall_protocol_interface;
    void* handle_protocol;
    void* reserved;
    void* register_protocol_notify;
    void* locate_handle;
    void* locate_device_path;
    void* install_configuration_table;

    void* load_image;
    void* start_image;
    void* exit_image;  // "Exit" — 이름 충돌 피하려 exit_image로 둔다.
    void* unload_image;
    efi_exit_boot_services_fn exit_boot_services;

    // 나머지(GetNextMonotonicCount 이후)는 이 파일이 아예 쓰지 않아
    // 정의하지 않는다 — 구조체 크기가 스펙보다 작아도, 우리가 실제로
    // 읽는 필드(위)의 오프셋만 맞으면 문제없다(뒤쪽 필드에는 접근하지
    // 않는다).
};

struct efi_configuration_table {
    efi_guid vendor_guid;
    void* vendor_table;
};

struct efi_system_table {
    efi_table_header hdr;
    uint16_t* firmware_vendor;
    uint32_t firmware_revision;
    efi_handle console_in_handle;
    void* con_in;
    efi_handle console_out_handle;
    void* con_out;
    efi_handle standard_error_handle;
    void* std_err;
    void* runtime_services;
    efi_boot_services* boot_services;
    uint64_t number_of_table_entries;
    efi_configuration_table* configuration_table;
};

constexpr efi_status k_efi_success = 0;
constexpr efi_status k_efi_error_bit = 0x8000000000000000ull;
constexpr efi_status k_efi_buffer_too_small = k_efi_error_bit | 5;

constexpr uint32_t k_efi_allocate_address = 2;  // EFI_ALLOCATE_TYPE.
constexpr uint32_t k_efi_loader_data = 2;        // EFI_MEMORY_TYPE.

constexpr uint64_t k_efi_page_size = 4096;

// ACPI 2.0+ RSDP를 가리키는 설정 테이블 GUID(우선), 없으면 1.0 GUID로
// 폴백한다(둘 다 UEFI 스펙에 고정된 잘 알려진 상수).
constexpr efi_guid k_acpi_20_table_guid = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
constexpr efi_guid k_acpi_table_guid = {
    0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

inline bool efi_guid_equal(const efi_guid& a, const efi_guid& b) {
    if (a.data1 != b.data1 || a.data2 != b.data2 || a.data3 != b.data3) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (a.data4[i] != b.data4[i]) {
            return false;
        }
    }
    return true;
}

}  // extern "C"
