// boot_info: 커널 코어와 initrun이 공유하는 유일한 부팅 관련 ABI
// (docs/spec/boot.md §3). 커널 코어는 이 구조체만 알고 Multiboot2/UEFI/FDT의
// 존재 자체를 모른다(ADR-002 HAL 경계) — 각 arch 계층이 자신의 부트
// 프로토콜을 파싱해 이 구조체로 변환한다.
#pragma once

#include <cstdint>

namespace boot {

constexpr uint64_t k_boot_info_magic = 0x4D434249ull;  // "MCBI"
constexpr uint32_t k_boot_info_version = 3;  // boot_device_descriptor 포함(ADR-131/146)

// M12(system-servers-bringup.md §M12, ADR-131 §결정2/ADR-146) — initrun이
// 부트 디바이스(virtio-blk)를 직접 마운트하는 데 필요한 최소 정보.
// initrd의 "disk.cfg" 엔트리(정확히 이 구조체 그대로의 바이트, OS
// 설치 시점에 채워지는 값 — 지금 개발 환경에서는 tools/mkinitrd.py가
// 그 절차를 대신한다)에서 온다. `valid==0`이면 disk.cfg가 없거나
// 읽지 못했다는 뜻 — initrun은 이 경우 부팅을 실패로 처리한다
// (ADR-131 §결정3의 PCIe 폴백 스캔은 M12 범위에서 구현하지 않는다 —
// 아래 §알려진 단순화 참고).
struct boot_device_descriptor {
    uint32_t valid;         // 0 = 없음/무효, 1 = 아래 필드 유효.
    uint32_t pci_bus;
    uint32_t pci_device;
    uint32_t pci_function;
    uint32_t fs_tag;        // 0 = cpio(newc). 그 외 값은 아직 없음.

    // 아래 두 필드는 disk.cfg에서 오지 않는다 — 커널이 부팅 중 실제로
    // PCI BAR를 배정한 뒤(ADR-147, kernel/arch/x86_64/pci_bringup.cpp)
    // 그 결과로 채운다. io_port_ok==0이면 initrun은 부팅을 실패로
    // 처리한다(이 커널은 legacy virtio-blk의 I/O 공간 BAR만 다룬다 —
    // 모던 virtio의 MMIO 공간 BAR는 지원하지 않는다).
    uint32_t io_port_ok;
    uint32_t io_port_base;
};

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

    boot_device_descriptor boot_device;  // ADR-131/146 — 위 주석 참고.
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
