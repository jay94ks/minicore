// x86_64 가상메모리 레이아웃 상수 (docs/spec/virtual-memory-layout.md §2/§5).
//
// k_physmap_base/k_physmap_size는 virtual-memory-layout.md §5가 "arch
// 계층이 정의해야 할 상수"로 명시한 부분이라 여기 남는다 —
// kernel/core/mm(M3, phys_map.hpp)이 실제 phys_to_virt/virt_to_phys를
// 소유하며, <arch_mm_defs.hpp>(제네릭 이름, kernel/core/mm이 include)를
// 통해 이 값을 재노출한다(arch_mm_defs.hpp 참고) — 값의 유일한 출처는
// 이 파일이다.
//
// image_virt_to_phys/k_kernel_phys_base는 커널 이미지 자신의 부트
// 배치(link.ld) 관련 상수라 physmap과는 별개의 관심사이며, 이 파일이
// 계속 소유한다(boot_info_x86_64.cpp가 커널/initrd 소유 영역을 마킹할
// 때 씀).
#pragma once

#include <cstdint>

namespace arch_x86_64 {

constexpr uint64_t k_physmap_base = 0xFFFF800000000000ull;
constexpr uint64_t k_physmap_size = 512ull * 1024 * 1024 * 1024;  // 512GiB

constexpr uint64_t k_kernel_virt_offset = 0xFFFFFFFF80000000ull;

// link.ld의 KERNEL_PHYS_BASE와 반드시 같은 값이어야 한다 — 링커
// 스크립트(다른 언어)와는 상수를 직접 공유할 수 없어 각자 정의하고
// 주석으로 동기화를 명시한다.
constexpr uint64_t k_kernel_phys_base = 0x00100000ull;

// 커널 이미지 자신(.text~.bss, higher-half) 안의 가상주소를 물리주소로
// 변환한다 — physmap과는 다른 주소 공간이므로 kernel/core/mm의
// phys_to_virt/virt_to_phys(physmap 전용)와 절대 혼용하지 않는다
// (link.ld의 AT() 규칙과 일치: LMA = VMA - offset).
inline uint64_t image_virt_to_phys(const void* image_addr) {
    return reinterpret_cast<uint64_t>(image_addr) - k_kernel_virt_offset;
}

}  // namespace arch_x86_64
