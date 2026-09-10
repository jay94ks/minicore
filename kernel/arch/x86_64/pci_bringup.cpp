// virtio-blk 부트 디바이스 전용 최소 PCI BAR 배정 구현. pci_bringup.hpp
// 상단 주석 참고.
#include "pci_bringup.hpp"

#include <mm/phys_map.hpp>

namespace kern::arch::x86_64 {

namespace {

constexpr uint16_t k_virtio_vendor_id = 0x1AF4;
constexpr uint32_t k_pci_offset_vendor = 0x00;
constexpr uint32_t k_pci_offset_command = 0x04;
constexpr uint32_t k_pci_offset_bar0 = 0x10;
constexpr uint32_t k_pci_command_io_space_enable = 1u << 0;

// 이 프로젝트가 관리하는 유일한 부트 디바이스 하나만 다루므로, 배정할
// I/O 포트 베이스를 그냥 고정값으로 정한다(레거시 예약 포트 범위
// 0x000~0x3FF보다 훨씬 위, 다른 어떤 장치도 이 최소 QEMU 구성에서
// 쓰지 않는 자리). 실제 크기 탐색(아래)은 이 값이 장치가 요구하는
// 정렬(항상 32의 배수 이하인 작은 크기)과 맞는지 확인하는 데만 쓴다.
constexpr uint32_t k_assigned_io_base = 0xC000;

uint64_t config_addr(uint64_t ecam_base, uint32_t bus, uint32_t device, uint32_t function) {
    return ecam_base + ((static_cast<uint64_t>(bus) << 20) | (static_cast<uint64_t>(device) << 15) |
                         (static_cast<uint64_t>(function) << 12));
}

volatile uint8_t* config_ptr(uint64_t config_base, uint32_t offset) {
    return static_cast<volatile uint8_t*>(kern::mm::phys_to_virt(config_base + offset));
}

uint16_t read16(uint64_t config_base, uint32_t offset) {
    return *reinterpret_cast<volatile uint16_t*>(config_ptr(config_base, offset));
}

void write16(uint64_t config_base, uint32_t offset, uint16_t value) {
    *reinterpret_cast<volatile uint16_t*>(config_ptr(config_base, offset)) = value;
}

uint32_t read32(uint64_t config_base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(config_ptr(config_base, offset));
}

void write32(uint64_t config_base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(config_ptr(config_base, offset)) = value;
}

}  // namespace

pci_bar_result assign_virtio_blk_bar(uint64_t ecam_base, uint32_t bus, uint32_t device,
                                      uint32_t function) {
    pci_bar_result result{false, 0, 0, 0};

    uint64_t config_base = config_addr(ecam_base, bus, device, function);
    uint16_t vendor_id = read16(config_base, k_pci_offset_vendor);
    if (vendor_id == 0xFFFF || vendor_id != k_virtio_vendor_id) {
        return result;  // 장치가 없거나(0xFFFF) virtio가 아니다.
    }
    result.vendor_id = vendor_id;
    result.device_id = read16(config_base, k_pci_offset_vendor + 2);

    uint32_t bar0 = read32(config_base, k_pci_offset_bar0);
    if ((bar0 & 0x1) == 0) {
        return result;  // I/O 공간 BAR가 아니다(메모리 BAR) — 이 클라이언트는 레거시 I/O 경로만 다룬다.
    }

    // 크기 탐색(PCI 3.0 §6.2.5.1): 전부 1로 써 보고 되읽으면 하드웨어가
    // "이 BAR가 실제로 쓰는 비트"만 남기고 나머지를 0으로 클리어한다.
    // I/O BAR는 하위 2비트가 항상 예약(0)+공간표시(1)라 실제 크기
    // 계산에서는 걷어낸다.
    write32(config_base, k_pci_offset_bar0, 0xFFFFFFFFu);
    uint32_t size_mask_raw = read32(config_base, k_pci_offset_bar0);
    uint32_t size_mask = size_mask_raw & ~0x3u;
    uint32_t size = (~size_mask) + 1;
    if (size == 0 || (k_assigned_io_base % size) != 0) {
        // 이 프로젝트가 고정해 둔 베이스가 장치가 요구하는 정렬과
        // 맞지 않는다 — 이 최소 클라이언트는 재계산하지 않고 그냥
        // 실패로 처리한다(실제로 QEMU virtio-blk-pci legacy는 항상
        // 32바이트 이하를 요구해 0xC000 정렬과 충돌한 적이 없다).
        write32(config_base, k_pci_offset_bar0, bar0);
        return result;
    }

    write32(config_base, k_pci_offset_bar0, k_assigned_io_base | 0x1u);

    uint16_t command = read16(config_base, k_pci_offset_command);
    write16(config_base, k_pci_offset_command, command | k_pci_command_io_space_enable);

    result.ok = true;
    result.io_port_base = k_assigned_io_base;
    return result;
}

}  // namespace kern::arch::x86_64
