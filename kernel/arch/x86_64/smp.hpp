// AP(Application Processor) 부팅 + IPI 기반 TLB shootdown (docs/plan/
// smp-fpu-bringup.md §M10, ADR-055).
#pragma once

#include <cstdint>

#include "acpi.hpp"

namespace arch_x86_64 {

// ap_trampoline.S가 가정하는 물리주소(같은 값으로 반드시 동기화 —
// ap_trampoline.S 상단 주석의 AP_TRAMPOLINE_BASE). SIPI 벡터 필드가
// 8비트라 1MiB 미만·4KiB 정렬이어야 한다.
constexpr uint64_t k_ap_trampoline_phys = 0x8000;
constexpr uint64_t k_ap_trampoline_size = 0x1000;

// MADT에서 얻은 결과로 BSP를 제외한 모든 AP를 하나씩 순차적으로
// 기동한다(ADR-055 — "즉시 전부", 다만 온라인 확인까지는 한 번에 하나씩
// 진행해 mailbox 하나로 충분하게 한다). lapic_init()이 이미 호출된
// 뒤, 그리고 mm::init()이 이미 끝난 뒤(AP 커널 스택을 이 함수가 직접
// 할당한다) 호출해야 한다.
void bring_up_aps(const madt_result& madt);

uint32_t online_cpu_count();

// page_table.cpp의 map_page/unmap_page/protect_page가 매핑을 바꿀
// 때마다 호출한다(계획 §M10 — "매핑을 바꿀 때마다 즉시 IPI 브로드캐스트").
// 온라인 AP가 없으면(기본, MINICORE_QEMU_SMP 미설정) 즉시 반환한다 —
// M1~M9 단일 코어 경로는 관찰 가능한 차이가 없다.
void broadcast_tlb_shootdown(uint64_t vaddr);

}  // namespace arch_x86_64

// idt.cpp(interrupt_dispatch)가 IPI 벡터에서 부르는 진입점.
extern "C" void smp_handle_tlb_shootdown_ipi();

// ap_trampoline.S(ap_entry64_highhalf)가 부르는 C++ 쪽 AP 진입점 —
// cpu_index는 bring_up_aps가 순서대로 부여한 논리 인덱스(BSP=0).
extern "C" void ap_main(uint32_t cpu_index);
