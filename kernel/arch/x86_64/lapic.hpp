// x86_64 Local APIC(LAPIC) 최소 드라이버 (docs/plan/smp-fpu-bringup.md
// §M10, ADR-055). xAPIC MMIO 인터페이스만 다룬다(x2APIC MSR 인터페이스는
// 이 계획의 범위 밖 — QEMU 기본 구성과 이 계획이 다루는 코어 수 규모
// (수 개)에서는 xAPIC로 충분하다).
#pragma once

#include <cstdint>

namespace kern::arch::x86_64 {

// base_phys는 acpi.hpp가 MADT(또는 Local APIC Address Override 엔트리)
// 에서 얻은 LAPIC MMIO 물리주소(기본값 0xFEE00000, Intel SDM Vol.3
// §11.4.1) — kern::mm::phys_to_virt로 physmap을 통해 접근한다(paging_setup.cpp가
// 부팅 시점에 이미 512GiB 전체를 항등 매핑해 두므로 이 MMIO 대상도
// 별도 매핑 없이 바로 접근 가능하다). BSP가 한 번만 호출한다(가상주소
// 매핑은 전역이라 코어마다 다시 계산할 필요 없다) — 이후 각 AP는
// lapic_enable_this_core()만 부른다.
void lapic_init(uint64_t base_phys);

// "로컬" APIC이라는 이름 그대로, 물리 MMIO 주소는 모든 코어가 공유해도
// 실제로 각 코어가 접근하는 레지스터 내용은 코어별로 다르다(하드웨어가
// 코어마다 라우팅) — 그래서 AP도 자기 몫으로 TPR=0·스퓨리어스 벡터
// 활성화를 각자 반복해야 한다. lapic_init()이 이미 BSP에서 호출되어
// g_lapic_base(가상주소)가 설정된 뒤에만 안전하다.
void lapic_enable_this_core();

// 이 코어의 LAPIC ID(APIC ID 레지스터, bits 24-31) — BSP 자신의 APIC ID를
// MADT의 어느 엔트리가 BSP인지 식별하는 데 쓴다.
uint32_t lapic_id();

// INIT-SIPI-SIPI 시퀀스(Intel MP 초기화 절차, ADR-055) — target_apic_id로
// 지정한 AP를 trampoline_phys(반드시 4KiB 정렬, < 1MiB — SIPI 벡터
// 필드가 8비트라 물리주소를 4KiB 단위로만 표현 가능하다)에서 실행
// 시작시킨다. 타이머가 없어(M10 범위 밖) 규격이 요구하는 지연은 고정
// 횟수 바쁜 대기로 흉내낸다.
void lapic_send_init_sipi_sipi(uint32_t target_apic_id, uint64_t trampoline_phys);

// 고정 벡터 IPI 1개를 target_apic_id에게 보낸다(TLB shootdown 등,
// smp.cpp가 호출).
void lapic_send_fixed_ipi(uint32_t target_apic_id, uint8_t vector);

// M21(general-purpose-completion.md §M21, ADR-176) — 이 코어의 LAPIC
// 타이머를 주기(periodic) 모드로 재프로그램해 vector로 반복 인터럽트를
// 건다. **BSP에서만 호출한다** — AP는 아직(M10/M11 결정 그대로)
// 협조적 스케줄러의 run_queue에 전혀 참여하지 않는다(kern::sched::current()가
// 코어별이 아니라 전역 하나뿐이다, scheduler.cpp). AP에서 이 함수를
// 부르면 그 코어의 타이머 틱이 BSP의 g_current를 잘못 건드리게 되므로
// (smp.cpp의 AP 진입부가 이 함수를 호출하지 않는 이유) 호출하지 않는다.
// initial_count/divide는 **보정 없는 값**이다(lapic.cpp busy_delay()와
// 같은 정신, YAGNI) — 실제 마이크로초 단위 보정(PIT/HPET)은 이
// 마일스톤 범위 밖(docs/design/open-items.md 참고).
void lapic_start_periodic_timer(uint8_t vector, uint32_t initial_count);

// M33(real-libc-syscall-layer.md §M33, ADR-184) — 이 코어의 LAPIC
// 타이머(divide=16 고정, 기존 lapic_start_periodic_timer()와 같은
// 전제)를 실측해, target_time_slice_us에 정확히 대응하는
// initial_count를 계산해 반환한다. hpet_available()(hpet.hpp)이면
// HPET을, 아니면 PIT(8254) 채널2 폴링(pit.hpp)을 기준시계로 삼는다.
// 이 함수 자신은 LVT_Timer/Initial_Count 레지스터를 측정이 끝나면
// 그대로 남겨 두지 않고 마스크(비활성)한 채로 되돌린다 — 실제로
// 주기 타이머를 켜고 끄는 것은 호출자(lapic_start_periodic_timer())
// 몫이다. 코어마다 1회 호출한다(BSP+각 AP, ADR-184 §결정1) — 이
// 함수 자신은 멀티코어 상태를 공유하지 않는다(레지스터가 코어별로
// 라우팅되는 LAPIC MMIO 특성 그대로, lapic_enable_this_core()와
// 같은 정신).
uint32_t calibrate_lapic_timer(uint32_t target_time_slice_us);

}  // namespace kern::arch::x86_64

// idt.cpp(interrupt_dispatch)가 부르는 EOI — extern "C"로 벡터 라우팅과
// 실제 드라이버 사이의 최소 결합만 남긴다(scheduler.cpp의
// arch_context_switch 선언과 같은 관례).
extern "C" void lapic_eoi();
