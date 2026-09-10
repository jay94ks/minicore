// AP(Application Processor) 부팅 + IPI 기반 TLB shootdown (docs/plan/
// smp-fpu-bringup.md §M10, ADR-055).
#pragma once

#include <cstdint>

#include "acpi.hpp"

namespace kern::arch::x86_64 {

// ap_trampoline.S가 가정하는 물리주소(같은 값으로 반드시 동기화 —
// ap_trampoline.S 상단 주석의 AP_TRAMPOLINE_BASE). SIPI 벡터 필드가
// 8비트라 1MiB 미만·4KiB 정렬이어야 한다.
constexpr uint64_t k_ap_trampoline_phys = 0x8000;
constexpr uint64_t k_ap_trampoline_size = 0x1000;

// MADT에서 얻은 결과로 BSP를 제외한 모든 AP를 하나씩 순차적으로
// 기동한다(ADR-055 — "즉시 전부", 다만 온라인 확인까지는 한 번에 하나씩
// 진행해 mailbox 하나로 충분하게 한다). lapic_init()이 이미 호출된
// 뒤, 그리고 kern::mm::init()이 이미 끝난 뒤(AP 커널 스택을 이 함수가 직접
// 할당한다) 호출해야 한다.
void bring_up_aps(const madt_result& madt);

uint32_t online_cpu_count();

// M11(ADR-036) — SRAT가 준 cpu→node 매핑을 apic_id로 색인해 저장한다.
// kernel_main이 acpi.cpp 파싱 직후, kern::sched::init() 이전에 한 번 호출한다
// — bring_up_aps()와는 독립적이다(코어 온라인 여부와 무관하게 매핑
// 자체는 항상 알 필요가 있다).
void set_cpu_node_map(const madt_result& madt, const srat_slit_result& srat);

// page_table.cpp의 map_page/unmap_page/protect_page가 매핑을 바꿀
// 때마다 호출한다(계획 §M10 — "매핑을 바꿀 때마다 즉시 IPI 브로드캐스트").
// 온라인 AP가 없으면(기본, MINICORE_QEMU_SMP 미설정) 즉시 반환한다 —
// M1~M9 단일 코어 경로는 관찰 가능한 차이가 없다.
void broadcast_tlb_shootdown(uint64_t vaddr);

}  // namespace kern::arch::x86_64

// idt.cpp(interrupt_dispatch)가 IPI 벡터에서 부르는 진입점.
extern "C" void smp_handle_tlb_shootdown_ipi();

// ap_trampoline.S(ap_entry64_highhalf)가 부르는 C++ 쪽 AP 진입점 —
// cpu_index는 bring_up_aps가 순서대로 부여한 논리 인덱스(BSP=0).
extern "C" void ap_main(uint32_t cpu_index);

// kernel/core/sched/scheduler.cpp(M11, ADR-036/053)가 부르는 HAL 훅 —
// "지금 이 코드를 실행 중인 코어가 속한 NUMA 노드 번호"를 반환한다.
// arch_context_switch/arch_idle_halt와 같은 관례(ADR-002 HAL 경계) —
// kernel/core/sched는 이 심벌의 정의를 몰라도 되고, x86_64 쪽만
// LAPIC ID를 읽어 set_cpu_node_map()이 저장해 둔 표를 찾아본다.
// set_cpu_node_map()이 아직 호출되지 않았거나 이 코어가 표에 없으면
// 노드 0으로 취급한다(토폴로지 정보 없음 폴백, boot.md §2와 동일한
// 정신).
extern "C" uint32_t arch_current_node_id();
