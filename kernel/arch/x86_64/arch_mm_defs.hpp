// x86_64 physmap 상수 (docs/spec/virtual-memory-layout.md §2/§5) —
// kernel/core/mm이 이 파일을 "arch_mm_defs.hpp"라는 제네릭 이름으로만
// include하고, 그것이 x86_64용이라는 사실을 모른다(ADR-002 HAL 경계) —
// kernel/CMakeLists.txt가 MINICORE_ARCH에 맞는 kernel/arch/<arch>/
// 디렉토리를 include 경로에 얹어 이름이 해석되게 한다.
//
// 값 자체는 memory_layout.hpp(부트 코드가 쓰는 arch 내부용 상수 모음)
// 것을 그대로 재노출한다 — 유일한 출처를 하나로 유지하기 위해서다.
#pragma once

#include <cstdint>

#include "memory_layout.hpp"

namespace arch_mm {

constexpr uint64_t k_physmap_base = arch_x86_64::k_physmap_base;
constexpr uint64_t k_physmap_size = arch_x86_64::k_physmap_size;

}  // namespace arch_mm
