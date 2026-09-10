// phys_to_virt/virt_to_phys — 물리 다이렉트맵(physmap) 경유 변환
// (docs/spec/virtual-memory-layout.md §5, ADR-078). kernel/core가
// 소유하는 정식 버전 — arch 계층은 <arch_mm_defs.hpp>로 상수
// (k_physmap_base/k_physmap_size)만 공개한다(ADR-002 HAL 경계).
//
// M1~M2에서는 이 파일이 아직 없어 kernel/arch/x86_64/memory_layout.hpp에
// 임시로 phys_to_virt를 뒀었다 — M3(이 파일)가 정식 위치다. boot 스텁
// (paging_setup.cpp)은 higher-half 매핑 자체를 만드는 코드라 이 함수를
// 쓸 수 없다(아직 매핑이 없는 시점에 실행되므로) — 그쪽은 계속 arch
// 로컬 상수를 직접 쓴다.
#pragma once

#include <cstdint>

#include <arch_mm_defs.hpp>
#include <k/panic.hpp>

namespace kern::mm {

inline void* phys_to_virt(uint64_t phys) {
    if (phys >= arch_mm::k_physmap_size) {
        LIBK_PANIC("phys_to_virt: address exceeds physmap range");
    }
    return reinterpret_cast<void*>(arch_mm::k_physmap_base + phys);
}

inline uint64_t virt_to_phys(const void* virtual_addr) {
    auto addr = reinterpret_cast<uint64_t>(virtual_addr);
    if (addr < arch_mm::k_physmap_base || addr >= arch_mm::k_physmap_base + arch_mm::k_physmap_size) {
        LIBK_PANIC("virt_to_phys: address outside physmap range");
    }
    return addr - arch_mm::k_physmap_base;
}

}  // namespace kern::mm
